#include "MDParticleContainer.H"
#include "Constants.H"

#include <thrust/reduce.h>

#include "md_K.H"

using namespace amrex;

namespace
{    
    void get_position_unit_cell(Real* r, const IntVect& nppc, int i_part)
    {
        int nx = nppc[0];
        int ny = nppc[1];
        int nz = nppc[2];
        
        int ix_part = i_part/(ny * nz);
        int iy_part = (i_part % (ny * nz)) % ny;
        int iz_part = (i_part % (ny * nz)) / ny;
        
        r[0] = (0.5+ix_part)/nx;
        r[1] = (0.5+iy_part)/ny;
        r[2] = (0.5+iz_part)/nz;
    }
    
    void get_gaussian_random_momentum(Real* u, Real u_mean, Real u_std) {
        Real ux_th = amrex::RandomNormal(0.0, u_std);
        Real uy_th = amrex::RandomNormal(0.0, u_std);
        Real uz_th = amrex::RandomNormal(0.0, u_std);
        
        u[0] = u_mean + ux_th;
        u[1] = u_mean + uy_th;
        u[2] = u_mean + uz_th;
    }
    
    Vector<Box> getBoundaryBoxes(const Box& box, const int ncells)
    {            
        AMREX_ASSERT_WITH_MESSAGE(box.size() > 2*ncells,
                                  "Too many cells requested in getBoundaryBoxes");
        
        AMREX_ASSERT_WITH_MESSAGE(box.isCellCentered(), 
                                  "Box must be cell-centered");
        
        Vector<Box> bl;
        for (int i = 0; i < AMREX_SPACEDIM; ++i) {
            BoxList face_boxes;
            Box hi_face_box = adjCellHi(box, i, ncells);
            Box lo_face_box = adjCellLo(box, i, ncells);
            face_boxes.push_back(hi_face_box); bl.push_back(hi_face_box);
            face_boxes.push_back(lo_face_box); bl.push_back(lo_face_box);
            for (auto face_box : face_boxes) {
                for (int j = 0; j < AMREX_SPACEDIM; ++j) {
                    if (i == j) continue;
                    BoxList edge_boxes;
                    Box hi_edge_box = adjCellHi(face_box, j, ncells);
                    Box lo_edge_box = adjCellLo(face_box, j, ncells);
                    edge_boxes.push_back(hi_edge_box); bl.push_back(hi_edge_box);
                    edge_boxes.push_back(lo_edge_box); bl.push_back(lo_edge_box);
                    for (auto edge_box : edge_boxes) {                    
                        for (int k = 0; k < AMREX_SPACEDIM; ++k) {
                            if ((j == k) or (i == k)) continue;
                            Box hi_corner_box = adjCellHi(edge_box, k, ncells);
                            Box lo_corner_box = adjCellLo(edge_box, k, ncells);
                            bl.push_back(hi_corner_box);
                            bl.push_back(lo_corner_box);
                        }
                    }
                }
            }
        }
        
        RemoveDuplicates(bl);
        return bl;
    }
}

MDParticleContainer::
MDParticleContainer(const Geometry            & a_geom,
                    const DistributionMapping & a_dmap,
                    const BoxArray            & a_ba)
    : ParticleContainer<PIdx::ncomps>(a_geom, a_dmap, a_ba)
{
    BL_PROFILE("MDParticleContainer::MDParticleContainer");

    buildNeighborMask();
}

void
MDParticleContainer::
buildNeighborMask()
{    
    BL_PROFILE("MDParticleContainer::buildNeighborMask");

    m_neighbor_mask_initialized = true;

    const int lev = 0;
    const Geometry& geom = this->Geom(lev);
    const BoxArray& ba = this->ParticleBoxArray(lev);
    const DistributionMapping& dmap = this->ParticleDistributionMap(lev);

    m_neighbor_mask_ptr.reset(new iMultiFab(ba, dmap, 1, 0));
    m_neighbor_mask_ptr->setVal(-1);

    const Periodicity& periodicity = geom.periodicity();
    const std::vector<IntVect>& pshifts = periodicity.shiftIntVect();

    for (MFIter mfi(ba, dmap); mfi.isValid(); ++mfi)
    {
        int grid = mfi.index();

        std::set<std::pair<int, Box> > neighbor_grids;
        for (auto pit=pshifts.cbegin(); pit!=pshifts.cend(); ++pit)
        {
            const Box box = ba[mfi] + *pit;

            const int nGrow = 1;
            const bool first_only = false;
            auto isecs = ba.intersections(box, first_only, nGrow);
        
            for (auto& isec : isecs)
            {
                int nbor_grid = isec.first;
                const Box isec_box = isec.second - *pit;
                if (grid != nbor_grid) neighbor_grids.insert(std::make_pair(nbor_grid, isec_box));
            }
        }
        
        BoxList isec_bl;
        std::vector<int> isec_grids;
        for (auto nbor_grid : neighbor_grids)
        {
            isec_grids.push_back(nbor_grid.first);
            isec_bl.push_back(nbor_grid.second);
        }
        BoxArray isec_ba(isec_bl);
        
        Vector<Box> bl = getBoundaryBoxes(amrex::grow(ba[mfi], -1), 1);
        
        m_grid_map[grid].resize(bl.size());
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
        {
            const Box& box = bl[i];
            
            const int nGrow = 0;
            const bool first_only = false;
            auto isecs = isec_ba.intersections(box, first_only, nGrow);

            if (! isecs.empty() ) (*m_neighbor_mask_ptr)[mfi].setVal(i, box);

            for (auto& isec : isecs)
            {
                m_grid_map[grid][i].push_back(isec_grids[isec.first]);
            }
        }
    }
}

void 
MDParticleContainer::
sortParticlesByNeighborDest()
{
    BL_PROFILE("MDParticleContainer::sortParticlesByNeighborDest");

    const int lev = 0;
    const auto& geom = Geom(lev);
    const auto dxi = Geom(lev).InvCellSizeArray();
    const auto plo = Geom(lev).ProbLoArray();
    const auto domain = Geom(lev).Domain();
    auto& plev  = GetParticles(lev);
    auto& ba = this->ParticleBoxArray(lev);
    auto& dmap = this->ParticleDistributionMap(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();        
        auto index = std::make_pair(gid, tid);

        const Box& bx = mfi.tilebox();
        const auto lo = amrex::lbound(bx);
        const auto hi = amrex::ubound(bx);

        auto& src_tile = plev[index];
        auto& aos   = src_tile.GetArrayOfStructs();
        const size_t np = aos.numParticles();

        Gpu::DeviceVector<int> neighbor_codes(np);
        int* pcodes = neighbor_codes.dataPtr();

        BaseFab<int>* mask_ptr = m_neighbor_mask_ptr->fabPtr(mfi);
        
        ParticleType* p_ptr = &(aos[0]);
        AMREX_FOR_1D ( np, i,
        {
	    IntVect iv = IntVect(
                AMREX_D_DECL(floor((p_ptr[i].pos(0)-plo[0])*dxi[0]),
                             floor((p_ptr[i].pos(1)-plo[1])*dxi[1]),
                             floor((p_ptr[i].pos(2)-plo[2])*dxi[2]))
                );
            
            iv += domain.smallEnd();
            
	    int code = (*mask_ptr)(iv);
            
            pcodes[i] = code;
        });

        thrust::sort_by_key(thrust::cuda::par(Cuda::The_ThrustCachedAllocator()),
                            neighbor_codes.begin(),
                            neighbor_codes.end(),
                            aos().begin());

        int num_codes = m_grid_map[gid].size();
        Gpu::DeviceVector<int> neighbor_code_begin(num_codes + 1);
        Gpu::DeviceVector<int> neighbor_code_end  (num_codes + 1);
        
        thrust::counting_iterator<int> search_begin(-1);
        thrust::lower_bound(thrust::device,
                            neighbor_codes.begin(),
                            neighbor_codes.end(),
                            search_begin,
                            search_begin + num_codes + 1,
                            neighbor_code_begin.begin());
        
        thrust::upper_bound(thrust::device,
                            neighbor_codes.begin(),
                            neighbor_codes.end(),
                            search_begin,
                            search_begin + num_codes + 1,
                            neighbor_code_end.begin());
        
        m_start[gid].resize(num_codes + 1);
        m_stop[gid].resize(num_codes + 1);
        
        Cuda::thrust_copy(neighbor_code_begin.begin(),
                          neighbor_code_begin.end(),
                          m_start[gid].begin());
        
        Cuda::thrust_copy(neighbor_code_end.begin(),
                          neighbor_code_end.end(),
                          m_stop[gid].begin());
        
        amrex::Print() << "Grid " << gid << " has \n";
        for (int i = 1; i < num_codes+1; ++i) {
            amrex::Print() << "\t" << m_stop[gid][i] - m_start[gid][i] << " particles for grids ";
            for (int j = 0; j < m_grid_map[gid][i-1].size(); ++j) {
                amrex::Print() << m_grid_map[gid][i-1][j] << " ";
            }
            amrex::Print() << "\n";
        }
        amrex::Print() << "\n";    

        for (int i = 1; i < num_codes + 1; ++i)
        {
            const size_t num_to_add = m_stop[gid][i] - m_start[gid][i];
            for (auto dst_grid : m_grid_map[gid][i-1]) {
                const int tid = 0;                
                auto pair_index = std::make_pair(dst_grid, tid);            
                const int dest_proc = dmap[dst_grid];
                if (dest_proc == ParallelDescriptor::MyProc())  // this is a local copy
                {
                    auto& dst_tile = plev[pair_index];
                    const int nRParticles = dst_tile.numRealParticles();
                    const int nNParticles = dst_tile.getNumNeighbors();
                    const int new_num_neighbors = nNParticles + num_to_add;
                    dst_tile.setNumNeighbors(new_num_neighbors);
                    
                    // copy structs
                    {
                        auto& src = src_tile.GetArrayOfStructs();
                        auto& dst = dst_tile.GetArrayOfStructs();
                        thrust::copy(thrust::device,
                                     src.begin() + m_start[gid][i], src.begin() + m_stop[gid][i], 
                                     dst().begin() + nRParticles);
                    }
                }
                else { // this is the non-local case
                    amrex::Abort("Not implemented yet.");
                }
            }
        }
    }
}

void
MDParticleContainer::
InitParticles(const IntVect& a_num_particles_per_cell,
              const Real     a_thermal_momentum_std,
              const Real     a_thermal_momentum_mean)
{
    BL_PROFILE("MDParticleContainer::InitParticles");

    amrex::Print() << "Generating particles... \n";

    const int lev = 0;   
    const Real* dx = Geom(lev).CellSize();
    const Real* plo = Geom(lev).ProbLo();
    
    const int num_ppc = AMREX_D_TERM( a_num_particles_per_cell[0],
                                     *a_num_particles_per_cell[1],
                                     *a_num_particles_per_cell[2]);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        const Box& tile_box  = mfi.tilebox();

        Cuda::HostVector<ParticleType> host_particles;
        
        for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv)) {
            for (int i_part=0; i_part<num_ppc;i_part++) {
                Real r[3];
                Real v[3];
                
                get_position_unit_cell(r, a_num_particles_per_cell, i_part);
                
                get_gaussian_random_momentum(v, a_thermal_momentum_mean,
                                             a_thermal_momentum_std);
                
                Real x = plo[0] + (iv[0] + r[0])*dx[0];
                Real y = plo[1] + (iv[1] + r[1])*dx[1];
                Real z = plo[2] + (iv[2] + r[2])*dx[2];
                
                ParticleType p;
                p.id()  = ParticleType::NextID();
                p.cpu() = ParallelDescriptor::MyProc();                
                p.pos(0) = x;
                p.pos(1) = y;
                p.pos(2) = z;
                
                p.rdata(PIdx::vx) = v[0];
                p.rdata(PIdx::vy) = v[1];
                p.rdata(PIdx::vz) = v[2];

                p.rdata(PIdx::ax) = 0.0;
                p.rdata(PIdx::ay) = 0.0;
                p.rdata(PIdx::az) = 0.0;
                
                host_particles.push_back(p);
            }
        }
        
        auto& particles = GetParticles(lev);
        auto& particle_tile = particles[std::make_pair(mfi.index(), mfi.LocalTileIndex())];
        auto old_size = particle_tile.GetArrayOfStructs().size();
        auto new_size = old_size + host_particles.size();
        particle_tile.resize(new_size);
        
        Cuda::thrust_copy(host_particles.begin(),
                          host_particles.end(),
                          particle_tile.GetArrayOfStructs().begin() + old_size);        
    }
}

void MDParticleContainer::BuildNeighborList()
{
    BL_PROFILE("MDParticleContainer::BuildNeighborList");

    const int lev = 0;
    const Geometry& geom = Geom(lev);
    const auto dxi = Geom(lev).InvCellSizeArray();
    const auto plo = Geom(lev).ProbLoArray();
    auto& plev  = GetParticles(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();        
        auto index = std::make_pair(gid, tid);

        const Box& bx = mfi.tilebox();
        const auto lo = amrex::lbound(bx);
        const auto hi = amrex::ubound(bx);

        auto& ptile = plev[index];
        auto& aos   = ptile.GetArrayOfStructs();
        const size_t np = aos.numParticles();

        Gpu::DeviceVector<unsigned int> cells(np);
        unsigned int* pcell = cells.dataPtr();

        Gpu::DeviceVector<unsigned int> counts(bx.numPts());
        unsigned int* pcount = counts.dataPtr();

        Gpu::DeviceVector<unsigned int> offsets(bx.numPts() + 1, np);
        unsigned int* poffset = offsets.dataPtr();

        Gpu::DeviceVector<unsigned int> permutation(np);
        unsigned int* pperm = permutation.dataPtr();

        // First we build the cell list data structure
        
        ParticleType* pstruct = &(aos[0]);
        AMREX_FOR_1D ( np, i,
        {
            int ix = (pstruct[i].pos(0)-plo[0])*dxi[0] - lo.x;
            int iy = (pstruct[i].pos(1)-plo[1])*dxi[1] - lo.y;
            int iz = (pstruct[i].pos(2)-plo[2])*dxi[2] - lo.z;
            int nx = hi.x-lo.x+1;
            int ny = hi.y-lo.y+1;
            int nz = hi.z-lo.z+1;            
            unsigned int uix = amrex::min(nx,amrex::max(0,ix));
            unsigned int uiy = amrex::min(ny,amrex::max(0,iy));
            unsigned int uiz = amrex::min(nz,amrex::max(0,iz));
            pcell[i] = (uix * ny + uiy) * nz + uiz; 
            Cuda::Atomic::Add(&pcount[pcell[i]], 1u);
        });

        thrust::exclusive_scan(counts.begin(), counts.end(), offsets.begin());

        thrust::copy(offsets.begin(), offsets.end()-1, counts.begin());

        constexpr unsigned int max_unsigned_int = std::numeric_limits<unsigned int>::max();

        AMREX_FOR_1D ( np, i,
        {
            unsigned int index = atomicInc(&pcount[pcell[i]], max_unsigned_int);
            pperm[index] = i;
        });

        // Now count the number of neighbors for each particle

        Gpu::DeviceVector<unsigned int> nbor_counts(np);
        unsigned int* pnbor_counts = nbor_counts.dataPtr();
        
        AMREX_FOR_1D ( np, i,
        {
            int ix = (pstruct[i].pos(0)-plo[0])*dxi[0] - lo.x;
            int iy = (pstruct[i].pos(1)-plo[1])*dxi[1] - lo.y;
            int iz = (pstruct[i].pos(2)-plo[2])*dxi[2] - lo.z;

            int nx = hi.x-lo.x+1;
            int ny = hi.y-lo.y+1;
            int nz = hi.z-lo.z+1;            

            int count = 0;

            for (int ii = amrex::max(ix-1, 0); ii <= amrex::min(ix+1, nx-1); ++ii) {
                for (int jj = amrex::max(iy-1, 0); jj <= amrex::min(iy+1, ny-1); ++jj) {
                    for (int kk = amrex::max(iz-1, 0); kk <= amrex::min(iz+1, nz-1); ++kk) {
                        int index = (ii * ny + jj) * nz + kk;
                        for (int p = poffset[index]; p < poffset[index+1]; ++p) {
                            if (pperm[p] == i) continue;
                            if (check_pair(pstruct[i], pstruct[pperm[p]]))
                                count += 1;
                        }
                    }
                }
            }
            
            pnbor_counts[i] = count;
        });

        // Now we can allocate and build our neighbor list

        const size_t total_nbors = thrust::reduce(nbor_counts.begin(), nbor_counts.end());
        m_nbor_offsets[index].resize(np + 1, total_nbors);
        unsigned int* pnbor_offset = m_nbor_offsets[index].dataPtr();

        thrust::exclusive_scan(nbor_counts.begin(), nbor_counts.end(),
                               m_nbor_offsets[index].begin());
                
        m_nbor_list[index].resize(total_nbors);
        unsigned int* pm_nbor_list = m_nbor_list[index].dataPtr();

        AMREX_FOR_1D ( np, i,
        {
            int ix = (pstruct[i].pos(0)-plo[0])*dxi[0] - lo.x;
            int iy = (pstruct[i].pos(1)-plo[1])*dxi[1] - lo.y;
            int iz = (pstruct[i].pos(2)-plo[2])*dxi[2] - lo.z;
            
            int nx = hi.x-lo.x+1;
            int ny = hi.y-lo.y+1;
            int nz = hi.z-lo.z+1;            

            int n = 0;            
            for (int ii = amrex::max(ix-1, 0); ii <= amrex::min(ix+1, nx-1); ++ii) {
                for (int jj = amrex::max(iy-1, 0); jj <= amrex::min(iy+1, ny-1); ++jj) {
                    for (int kk = amrex::max(iz-1, 0); kk <= amrex::min(iz+1, nz-1); ++kk) {
                        int index = (ii * ny + jj) * nz + kk;
                        for (int p = poffset[index]; p < poffset[index+1]; ++p) {
                            if (pperm[p] == i) continue;
                            if (check_pair(pstruct[i], pstruct[pperm[p]])) {
                                pm_nbor_list[pnbor_offset[i] + n] = pperm[p]; 
                                ++n;
                            }
                        }
                    }
                }
            }
        });
    }
}

void MDParticleContainer::printNeighborList()
{
    BL_PROFILE("MDParticleContainer::printNeighborList");

    const int lev = 0;
    const Geometry& geom = Geom(lev);
    auto& plev  = GetParticles(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();
        auto index = std::make_pair(gid, tid);

        auto& ptile = plev[index];
        auto& aos   = ptile.GetArrayOfStructs();
        const size_t np = aos.numParticles();

        Gpu::HostVector<unsigned int> host_nbor_offsets(m_nbor_offsets[index].size());
        Gpu::HostVector<unsigned int> host_nbor_list(m_nbor_list[index].size());

        Cuda::thrust_copy(m_nbor_offsets[index].begin(),
                          m_nbor_offsets[index].end(),
                          host_nbor_offsets.begin());

        Cuda::thrust_copy(m_nbor_list[index].begin(),
                          m_nbor_list[index].end(),
                          host_nbor_list.begin());

        for (int i = 0; i < np; ++i) {
            amrex::Print() << "Particle " << i << " will collide with: ";
            for (int j = host_nbor_offsets[i]; j < host_nbor_offsets[i+1]; ++j) {
                amrex::Print() << host_nbor_list[j] << " ";
            }
            amrex::Print() << "\n";
        }
    }
}

void MDParticleContainer::computeForces()
{
    BL_PROFILE("MDParticleContainer::computeForces");

    const int lev = 0;
    const Geometry& geom = Geom(lev);
    auto& plev  = GetParticles(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();
        auto index = std::make_pair(gid, tid);

        auto& ptile = plev[index];
        auto& aos   = ptile.GetArrayOfStructs();
        const size_t np = aos.numParticles();

        unsigned int* pnbor_offset = m_nbor_offsets[index].dataPtr();
        unsigned int* pm_nbor_list = m_nbor_list[index].dataPtr();
        ParticleType* pstruct = &(aos[0]);

       // now we loop over the neighbor list and compute the forces
        AMREX_FOR_1D ( np, i,
        {
            pstruct[i].rdata(PIdx::ax) = 0.0;
            pstruct[i].rdata(PIdx::ay) = 0.0;
            pstruct[i].rdata(PIdx::az) = 0.0;

            for (int k = pnbor_offset[i]; k < pnbor_offset[i+1]; ++k) {
                int j = pm_nbor_list[k];
                
                Real dx = pstruct[i].pos(0) - pstruct[j].pos(0);
                Real dy = pstruct[i].pos(1) - pstruct[j].pos(1);
                Real dz = pstruct[i].pos(2) - pstruct[j].pos(2);

                Real r2 = dx*dx + dy*dy + dz*dz;
                r2 = amrex::max(r2, Params::min_r*Params::min_r);
                Real r = sqrt(r2);

                Real coef = (1.0 - Params::cutoff / r) / r2 / Params::mass;
                pstruct[i].rdata(PIdx::ax) += coef * dx;
                pstruct[i].rdata(PIdx::ay) += coef * dy;
                pstruct[i].rdata(PIdx::az) += coef * dz;                
            }
        });
    }
}

void MDParticleContainer::moveParticles(const amrex::Real& dt)
{
    BL_PROFILE("MDParticleContainer::moveParticles");

    const int lev = 0;
    const Geometry& geom = Geom(lev);
    const auto plo = Geom(lev).ProbLoArray();
    const auto phi = Geom(lev).ProbHiArray();
    auto& plev  = GetParticles(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();
        
        auto& ptile = plev[std::make_pair(gid, tid)];
        auto& aos   = ptile.GetArrayOfStructs();
        ParticleType* pstruct = &(aos[0]);

        const size_t np = aos.numParticles();
    
        // now we move the particles
        AMREX_FOR_1D ( np, i,
        {
            pstruct[i].rdata(PIdx::vx) += pstruct[i].rdata(PIdx::ax) * dt;
            pstruct[i].rdata(PIdx::vy) += pstruct[i].rdata(PIdx::ay) * dt;
            pstruct[i].rdata(PIdx::vz) += pstruct[i].rdata(PIdx::az) * dt;

            pstruct[i].pos(0) += pstruct[i].rdata(PIdx::vx) * dt;
            pstruct[i].pos(1) += pstruct[i].rdata(PIdx::vy) * dt;
            pstruct[i].pos(2) += pstruct[i].rdata(PIdx::vz) * dt;

            for (int idim = 0; idim < AMREX_SPACEDIM; ++idim) {
                while ( (pstruct[i].pos(idim) < plo[idim]) or (pstruct[i].pos(idim) > phi[idim]) ) {
                    if ( pstruct[i].pos(idim) < plo[idim] ) {
                        pstruct[i].pos(idim) = 2*plo[idim] - pstruct[i].pos(idim);
                    } else {
                        pstruct[i].pos(idim) = 2*phi[idim] - pstruct[i].pos(idim);
                    }
                    pstruct[i].rdata(idim) *= -1; // flip velocity
                }
            }
        });
    }
}
