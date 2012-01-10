module nodal_sync_resid_module
  use bl_constants_module
  implicit none

contains

  subroutine get_sigma_2d(sig, msk, ac)
    real(kind=dp_t), intent(inout) :: sig(-1:,-1:)
    real(kind=dp_t), intent(in   ) :: msk(-1:,-1:)
    real(kind=dp_t), intent(in   ) :: ac (-1:,-1:)
    
    integer :: i, j, nx, ny

    nx = size(msk,dim=1) - 2
    ny = size(msk,dim=2) - 2

    do j = 0, ny-1
       do i = 0, nx-1
          sig(i,j) = ac(i,j) * msk(i,j)
       end do
    end do

  end subroutine get_sigma_2d

  subroutine add_Dv_crse_2d(res, msk, vel, dx)
    real(kind=dp_t), intent(inout) :: res(-1:,-1:)
    real(kind=dp_t), intent(in   ) :: msk(-1:,-1:)
    real(kind=dp_t), intent(in   ) :: vel(-1:,-1:,:)
    real(kind=dp_t), intent(in   ) :: dx(:)
    
    integer :: i, j, nx, ny
    real(kind=dp_t) :: divv

    nx = size(msk,dim=1) - 2
    ny = size(msk,dim=2) - 2

    do j = 0, ny
       do i = 0, nx
          if ( any(msk(i-1:i,j-1:j) .eq. ZERO) .and. &
               any(msk(i-1:i,j-1:j) .eq. ONE ) .and. &
               all(msk(i-1:i,j-1:j) .ge. ZERO) ) then
             divv = (vel(i  ,j,1)*msk(i , j) + vel(i  ,j-1,1)*msk(i  ,j-1) &
                  -  vel(i-1,j,1)*msk(i-1,j) - vel(i-1,j-1,1)*msk(i-1,j-1)) / dx(1) &
                  + (vel(i,j  ,2)*msk(i,j  ) + vel(i-1,j  ,2)*msk(i-1,j  ) &
                  -  vel(i,j-1,2)*msk(i,j-1) - vel(i-1,j-1,2)*msk(i-1,j-1)) / dx(2)
             divv = HALF * divv
             res(i,j) = -res(i,j) + divv  ! Yes! -res(i,j) because of the sign of stencil
          else
             res(i,j) = ZERO
          end if
       end do
    end do

  end subroutine add_Dv_crse_2d

  subroutine divuo_2d(dvo, msk, vel, dx)
    real(kind=dp_t), intent(inout) :: dvo(-1:,-1:)
    real(kind=dp_t), intent(in   ) :: msk(-1:,-1:)
    real(kind=dp_t), intent(in   ) :: vel(-1:,-1:,:)
    real(kind=dp_t), intent(in   ) :: dx(:)
    
    integer :: i, j, nx, ny
    real(kind=dp_t) :: divv

    nx = size(msk,dim=1) - 2
    ny = size(msk,dim=2) - 2

    do j = 0, ny
       do i = 0, nx
          divv = (vel(i  ,j,1)*msk(i , j) + vel(i  ,j-1,1)*msk(i  ,j-1) &
               -  vel(i-1,j,1)*msk(i-1,j) - vel(i-1,j-1,1)*msk(i-1,j-1)) / dx(1) &
               + (vel(i,j  ,2)*msk(i,j  ) + vel(i-1,j  ,2)*msk(i-1,j  ) &
               -  vel(i,j-1,2)*msk(i,j-1) - vel(i-1,j-1,2)*msk(i-1,j-1)) / dx(2)
          dvo(i,j) = HALF * divv
       end do
    end do

  end subroutine divuo_2d

  subroutine get_sync_res_2d(res, dvo, msk)
    real(kind=dp_t), intent(inout) :: res(-1:,-1:)
    real(kind=dp_t), intent(in   ) :: dvo(-1:,-1:)
    real(kind=dp_t), intent(in   ) :: msk(-1:,-1:)
    
    integer :: i, j, nx, ny

    nx = size(msk,dim=1) - 2
    ny = size(msk,dim=2) - 2

    do j = 0, ny
       do i = 0, nx
          if (all(msk(i-1:i,j-1:j) .eq. ONE)) then
             res(i,j) = ZERO
          else if (all(msk(i-1:i,j-1:j) .eq. ZERO)) then
             res(i,j) = ZERO
          else if (any(msk(i-1:i,j-1:j) .lt. ZERO)) then
             res(i,j) = ZERO
          else
             res(i,j) = res(i,j) + dvo(i,j)
          end if
       end do
    end do

  end subroutine get_sync_res_2d

end module nodal_sync_resid_module


subroutine mgt_alloc_nodal_sync()
  use nodal_cpp_mg_module
  implicit none
  logical,dimension(3) :: nodal

  allocate(mgts%sync_res(1))
  allocate(mgts%sync_msk(1))
  allocate(mgts%vold(1))

  nodal = .true.

  call build(mgts%sync_res(1) , mgts%mla%la(1), nc = 1, ng = 1, nodal = nodal)
  call build(mgts%sync_msk(1) , mgts%mla%la(1), nc = 1, ng = 1)
  call build(mgts%vold(1)     , mgts%mla%la(1), nc = mgts%dim, ng = 1)

  call setval(mgts%sync_res(1),ZERO,all=.true.)
  call setval(mgts%vold(1),ZERO,all=.true.)
  
end subroutine mgt_alloc_nodal_sync

subroutine mgt_dealloc_nodal_sync()
  use nodal_cpp_mg_module
  implicit none
  
  call destroy(mgts%sync_res(1))
  call destroy(mgts%sync_msk(1))
  call destroy(mgts%vold(1))

  deallocate(mgts%sync_res)
  deallocate(mgts%sync_msk)
  deallocate(mgts%vold)

end subroutine mgt_dealloc_nodal_sync

subroutine mgt_set_sync_msk_1d(lev, n, msk_in, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(1), hi(1), plo(1), phi(1)
  real(kind=dp_t), intent(in) :: msk_in(plo(1):phi(1))
  real(kind=dp_t), pointer :: mskp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  mskp => dataptr(mgts%sync_msk(flev), fn)
  mskp(plo(1):phi(1),1,1,1) = msk_in(plo(1):phi(1))
end subroutine mgt_set_sync_msk_1d

subroutine mgt_set_sync_msk_2d(lev, n, msk_in, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(2), hi(2), plo(2), phi(2)
  real(kind=dp_t), intent(in) :: msk_in(plo(1):phi(1),plo(2):phi(2))
  real(kind=dp_t), pointer :: mskp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  mskp => dataptr(mgts%sync_msk(flev), fn)
  mskp(plo(1):phi(1),plo(2):phi(2),1,1) = msk_in(plo(1):phi(1),plo(2):phi(2))
end subroutine mgt_set_sync_msk_2d

subroutine mgt_set_sync_msk_3d(lev, n, msk_in, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(3), hi(3), plo(3), phi(3)
  real(kind=dp_t), intent(in) :: msk_in(plo(1):phi(1),plo(2):phi(2),plo(3):phi(3))
  real(kind=dp_t), pointer :: mskp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  mskp => dataptr(mgts%sync_msk(flev), fn)
  mskp(plo(1):phi(1),plo(2):phi(2),plo(3):phi(3),1) = &
       msk_in(plo(1):phi(1),plo(2):phi(2),plo(3):phi(3))
end subroutine mgt_set_sync_msk_3d

subroutine mgt_set_vold_1d(lev, n, v_in, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(1), hi(1), plo(1), phi(1)
  real(kind=dp_t), intent(in) :: v_in(plo(1):phi(1))
  real(kind=dp_t), pointer :: vp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  vp => dataptr(mgts%vold(flev), fn)
  vp(lo(1):hi(1),1,1,1) = v_in(lo(1):hi(1))
end subroutine mgt_set_vold_1d

subroutine mgt_set_vold_2d(lev, n, v_in, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(2), hi(2), plo(2), phi(2)
  real(kind=dp_t), intent(in) :: v_in(plo(1):phi(1),plo(2):phi(2),1:2)
  real(kind=dp_t), pointer :: vp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  vp => dataptr(mgts%vold(flev), fn)
  vp(lo(1):hi(1),lo(2):hi(2),1,1:2) = v_in(lo(1):hi(1),lo(2):hi(2),1:2)
end subroutine mgt_set_vold_2d

subroutine mgt_set_vold_3d(lev, n, v_in, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(3), hi(3), plo(3), phi(3)
  real(kind=dp_t), intent(in) :: v_in(plo(1):phi(1),plo(2):phi(2),plo(3):phi(3),1:3)
  real(kind=dp_t), pointer :: vp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  vp => dataptr(mgts%vold(flev), fn)
  vp(lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),1:3) = &
       v_in(lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),1:3)
end subroutine mgt_set_vold_3d

subroutine mgt_get_sync_res_1d(lev, n, res, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(1), hi(1), plo(1), phi(1)
  real(kind=dp_t), intent(inout) :: res(plo(1):phi(1))
  real(kind=dp_t), pointer :: rp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  rp => dataptr(mgts%sync_res(flev), fn)
  res(lo(1):hi(1)) = rp(lo(1):hi(1), 1, 1, 1)

end subroutine mgt_get_sync_res_1d

subroutine mgt_get_sync_res_2d(lev, n, res, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(2), hi(2), plo(2), phi(2)
  real(kind=dp_t), intent(inout) :: res(plo(1):phi(1), plo(2):phi(2))
  real(kind=dp_t), pointer :: rp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  rp => dataptr(mgts%sync_res(flev), fn)
  res(lo(1):hi(1), lo(2):hi(2)) = rp(lo(1):hi(1), lo(2):hi(2), 1, 1)

end subroutine mgt_get_sync_res_2d

subroutine mgt_get_sync_res_3d(lev, n, res, plo, phi, lo, hi)
  use nodal_cpp_mg_module
  implicit none
  integer, intent(in) :: lev, n, lo(3), hi(3), plo(3), phi(3)
  real(kind=dp_t), intent(inout) :: res(plo(1):phi(1), plo(2):phi(2), plo(3):phi(3))
  real(kind=dp_t), pointer :: rp(:,:,:,:)
  integer :: flev, fn
  fn = n + 1
  flev = lev+1

  rp => dataptr(mgts%sync_res(flev), fn)
  res(lo(1):hi(1), lo(2):hi(2), lo(3):hi(3)) =  &
       rp(lo(1):hi(1), lo(2):hi(2), lo(3):hi(3), 1)

end subroutine mgt_get_sync_res_3d

subroutine mgt_compute_sync_resid_crse()
  use nodal_cpp_mg_module
  use nodal_sync_resid_module
  use nodal_stencil_fill_module, only : stencil_fill_nodal
  use itsol_module, only : itsol_stencil_apply
  implicit none

  integer :: i, dm, mglev
  real(kind=dp_t), pointer :: res(:,:,:,:) 
  real(kind=dp_t), pointer :: msk(:,:,:,:) 
  real(kind=dp_t), pointer :: vo(:,:,:,:) 
  real(kind=dp_t), pointer :: ac(:,:,:,:) 
  real(kind=dp_t), pointer :: sig(:,:,:,:) 
  type(multifab) :: sigma

  dm = get_dim(mgts%sync_res(1))
  mglev = mgts%mgt(1)%nlevels

  call build(sigma, mgts%mla%la(1), nc=1, ng=1)
  call setval(sigma,ZERO,all=.true.)

  do i = 1, nboxes(sigma)
     if (remote(sigma, i)) cycle
     sig => dataptr(sigma, i)
     msk => dataptr(mgts%sync_msk(1), i)
     ac  => dataptr(mgts%amr_coeffs(1), i)
     select case (dm)
     case (1)
        call bl_error('mgt_compute_sync_resid_crse: 1d not done')
     case (2)
        call get_sigma_2d(sig(:,:,1,1), msk(:,:,1,1), ac(:,:,1,1))
     case (3)
        call bl_error('mgt_compute_sync_resid_crse: 3d not done')
     end select
  end do

  call multifab_fill_boundary(sigma)

  call stencil_fill_nodal(mgts%mgt(1)%ss(mglev), sigma, mgts%mgt(1)%dh(:,mglev), &
       mgts%mgt(1)%mm(mglev), mgts%mgt(1)%face_type, mgts%stencil_type)

  call itsol_stencil_apply(mgts%mgt(1)%ss(mglev), mgts%sync_res(1), mgts%uu(1), &
       mgts%mgt(1)%mm(mglev), mgts%mgt(1)%uniform_dh)

  do i = 1, nboxes(mgts%sync_res(1))
     if (remote(mgts%sync_res(1), i)) cycle
     res => dataptr(mgts%sync_res(1), i)
     msk => dataptr(mgts%sync_msk(1), i)
     vo  => dataptr(mgts%vold    (1), i)
     select case (dm)
     case (1)
        call bl_error('mgt_compute_sync_resid_crse: 1d not done')
     case (2)
        call add_Dv_crse_2d(res(:,:,1,1), msk(:,:,1,1), vo(:,:,1,:), mgts%mgt(1)%dh(:,mglev))
     case (3)
        call bl_error('mgt_compute_sync_resid_crse: 3d not done')
     end select
  end do

  call destroy(sigma)

end subroutine mgt_compute_sync_resid_crse

subroutine mgt_compute_sync_resid_fine()
  use nodal_cpp_mg_module
  use nodal_sync_resid_module
  use ml_nd_module
  use nodal_stencil_fill_module, only : stencil_fill_nodal, stencil_fill_one_sided
  implicit none

  integer :: i, dm, mglev, ng
  real(kind=dp_t), pointer :: res(:,:,:,:) 
  real(kind=dp_t), pointer :: msk(:,:,:,:) 
  real(kind=dp_t), pointer :: vo(:,:,:,:) 
  real(kind=dp_t), pointer :: dvo(:,:,:,:) 
  type(multifab) :: ss1
  type(multifab) :: divuo
  type(multifab) :: rh0  
  logical :: nodal(3)

  nodal = .true.
  dm = get_dim(mgts%sync_res(1))
  mglev = mgts%mgt(1)%nlevels
  ng    = nghost(mgts%vold(1))

  call build(divuo, mgts%mla%la(1), nc=1, ng=1, nodal=nodal)
  call build(rh0, mgts%mla%la(1), nc=1, ng=1, nodal=nodal)
  call setval(rh0,ZERO,all=.true.)

  do i = 1, nboxes(divuo)
     if (remote(divuo, i)) cycle
     dvo => dataptr(divuo, i)
     msk => dataptr(mgts%sync_msk(1), i)
     vo  => dataptr(mgts%vold    (1), i)
     select case (dm)
     case (1)
        call bl_error('mgt_compute_sync_resid_crse: 1d not done')
     case (2)
        call divuo_2d(dvo(:,:,1,1), msk(:,:,1,1), vo(:,:,1,:), mgts%mgt(1)%dh(:,mglev))
     case (3)
        call bl_error('mgt_compute_sync_resid_crse: 3d not done')
     end select
  end do

  if (mgts%stencil_type .eq. ST_CROSS) then
     call multifab_build(ss1, mgts%mla%la(1), 2*dm+1, 0, nodal, stencil=.true.)
     call stencil_fill_one_sided(ss1, mgts%amr_coeffs(1), mgts%mgt(1)%dh(:,mglev), &
          mgts%mgt(1)%mm(mglev), mgts%mgt(1)%face_type)

     call grid_res(ss1, &
          mgts%sync_res(1), rh0, mgts%uu(1), mgts%mgt(1)%mm(mglev), &
          mgts%mgt(1)%face_type, mgts%mgt(1)%uniform_dh)
  else
     call grid_res(mgts%mgt(1)%ss(mglev), &
          mgts%sync_res(1), rh0, mgts%uu(1), mgts%mgt(1)%mm(mglev), &
          mgts%mgt(1)%face_type, mgts%mgt(1)%uniform_dh)
  endif

  do i = 1, nboxes(mgts%sync_res(1))
     if (remote(mgts%sync_res(1), i)) cycle
     res => dataptr(mgts%sync_res(1), i)
     dvo => dataptr(divuo, i)
     msk => dataptr(mgts%sync_msk(1), i)
     select case (dm)
     case (1)
        call bl_error('mgt_compute_sync_resid_fine: 1d not done')
     case (2)
        call get_sync_res_2d(res(:,:,1,1), dvo(:,:,1,1), msk(:,:,1,1))
     case (3)
        call bl_error('mgt_compute_sync_resid_fine: 3d not done')
     end select
  end do

  if (mgts%stencil_type .eq. ST_CROSS) then
     call destroy(ss1)
  endif

  call destroy(divuo)

end subroutine mgt_compute_sync_resid_fine

