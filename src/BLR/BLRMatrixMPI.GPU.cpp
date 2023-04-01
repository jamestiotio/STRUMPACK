/*
 * STRUMPACK -- STRUctured Matrices PACKage, Copyright (c) 2014, The
 * Regents of the University of California, through Lawrence Berkeley
 * National Laboratory (subject to receipt of any required approvals
 * from the U.S. Dept. of Energy).  All rights reserved.
 *
 * If you have questions about your rights to use or distribute this
 * software, please contact Berkeley Lab's Technology Transfer
 * Department at TTD@lbl.gov.
 *
 * NOTICE. This software is owned by the U.S. Department of Energy. As
 * such, the U.S. Government has been granted for itself and others
 * acting on its behalf a paid-up, nonexclusive, irrevocable,
 * worldwide license in the Software to reproduce, prepare derivative
 * works, and perform publicly and display publicly.  Beginning five
 * (5) years after the date permission to assert copyright is obtained
 * from the U.S. Department of Energy, and subject to any subsequent
 * five (5) year renewals, the U.S. Government is granted for itself
 * and others acting on its behalf a paid-up, nonexclusive,
 * irrevocable, worldwide license in the Software to reproduce,
 * prepare derivative works, distribute copies to the public, perform
 * publicly and display publicly, and to permit others to do so.
 *
 * Developers: Pieter Ghysels, Francois-Henry Rouet, Xiaoye S. Li.
 *             (Lawrence Berkeley National Lab, Computational Research
 *             Division).
 *
 */
#include <cassert>
#include <memory>
#include <functional>
#include <algorithm>

#include "misc/TaskTimer.hpp"

#include "BLRMatrixMPI.hpp"
#include "BLRTileBLAS.hpp"
#include "BLRBatch.hpp"

#if defined(STRUMPACK_USE_CUDA)
#include "dense/CUDAWrapper.hpp"
#endif
#if defined(STRUMPACK_USE_HIP)
#include "dense/HIPWrapper.hpp"
#endif

#include "sparse/fronts/FrontalMatrixGPUKernels.hpp"

namespace strumpack {
  namespace BLR {

    template<typename scalar_t> void
    BLRMatrixMPI<scalar_t>::move_to_gpu(gpu::Stream& s,
                                        scalar_t* dptr) {
      for (std::size_t j=0; j<colblocks(); j++) {
        if (!grid()->is_local_col(j)) continue;
        for (std::size_t i=0; i<rowblocks(); i++) {
          if (!grid()->is_local_row(i)) continue;
          tile(i, j).move_to_gpu(s, dptr);
        }
      }
    }

    template<typename scalar_t> void
    BLRMatrixMPI<scalar_t>::move_to_cpu(gpu::Stream& s,
                                        scalar_t* pinned) {
      for (std::size_t j=0; j<colblocks(); j++) {
        if (!grid()->is_local_col(j)) continue;
        for (std::size_t i=0; i<rowblocks(); i++) {
          if (!grid()->is_local_row(i)) continue;
          tile(i, j).move_to_cpu(s, pinned);
        }
      }
    }


    template<typename scalar_t> DenseTile<scalar_t>
    BLRMatrixMPI<scalar_t>::bcast_dense_tile_along_row_gpu
    (std::size_t i, std::size_t j,
     gpu::Stream& stream, scalar_t* dptr, scalar_t* pinned) const {
      DenseTile<scalar_t> t(tilerows(i), tilecols(j));
      int src = j % grid()->npcols();
      auto& c = grid()->row_comm();
      // TODO CUDA aware MPI
      if (c.rank() == src)
        gpu_check(gpu::copy_device_to_host(t.D(), tile(i, j).D()));
      c.broadcast_from(t.D().data(), t.rows()*t.cols(), src);
      t.move_to_gpu(stream, dptr);
      return t;
    }
    template<typename scalar_t> DenseTile<scalar_t>
    BLRMatrixMPI<scalar_t>::bcast_dense_tile_along_col_gpu
    (std::size_t i, std::size_t j,
     gpu::Stream& stream, scalar_t* dptr, scalar_t* pinned) const {
      DenseTile<scalar_t> t(tilerows(i), tilecols(j));
      int src = i % grid()->nprows();
      auto& c = grid()->col_comm();
      // TODO CUDA aware MPI
      if (c.rank() == src)
        gpu_check(gpu::copy_device_to_host(t.D(), tile(i, j).D()));
      c.broadcast_from(t.D().data(), t.rows()*t.cols(), src);
      t.move_to_gpu(stream, dptr);
      return t;
    }

    template<typename scalar_t>
    std::vector<std::unique_ptr<BLRTile<scalar_t>>>
    BLRMatrixMPI<scalar_t>::bcast_row_of_tiles_along_cols_gpu
    (std::size_t i, std::size_t j0, std::size_t j1,
     gpu::Stream& stream, scalar_t* dptr, scalar_t* pinned) const {
      if (!grid()) return {};
      int src = i % grid()->nprows();
      std::size_t msg_size = 0, nr_tiles = 0;
      std::vector<std::int64_t> ranks;
      if (grid()->is_local_row(i)) {
        for (std::size_t j=j0; j<j1; j++)
          if (grid()->is_local_col(j)) {
            auto& t = tile(i, j);
            msg_size += t.nonzeros();
            ranks.push_back(t.is_low_rank() ? t.rank() : -1);
            nr_tiles++;
          }
      } else {
        for (std::size_t j=j0; j<j1; j++)
          if (grid()->is_local_col(j))
            nr_tiles++;
        ranks.resize(nr_tiles);
      }
      std::vector<std::unique_ptr<BLRTile<scalar_t>>> Tij;
      if (ranks.empty()) return Tij;
      ranks.push_back(msg_size);
      grid()->col_comm().broadcast_from(ranks, src);
      msg_size = ranks.back();
      // use HostMemory, and reuse?
      std::vector<scalar_t> buf(msg_size);
      auto ptr = buf.data();
      if (grid()->is_local_row(i)) {
        for (std::size_t j=j0; j<j1; j++)
          if (grid()->is_local_col(j)) {
            auto& t = tile(i, j);
            if (t.is_low_rank()) {
              gpu_check(gpu::copy_device_to_host
                        (ptr, t.U().data(), t.U().nonzeros()));
              ptr += t.U().nonzeros();
              gpu_check(gpu::copy_device_to_host
                        (ptr, t.V().data(), t.V().nonzeros()));
              ptr += t.V().nonzeros();
            } else {
              gpu_check(gpu::copy_device_to_host
                        (ptr, t.D().data(), t.D().nonzeros()));
              ptr += t.D().nonzeros();
            }
          }
      }
      // TODO CUDA aware MPI
      grid()->col_comm().broadcast_from(buf, src);
      Tij.reserve(nr_tiles);
      ptr = buf.data();
      auto m = tilerows(i);
      for (std::size_t j=j0; j<j1; j++)
        if (grid()->is_local_col(j)) {
          auto r = ranks[Tij.size()];
          auto n = tilecols(j);
          if (r != -1) {
            auto t = new LRTile<scalar_t>(m, n, r);
            // this could be copied straight to the GPU
            std::copy(ptr, ptr+m*r, t->U().data());  ptr += m*r;
            std::copy(ptr, ptr+r*n, t->V().data());  ptr += r*n;
            t->move_to_gpu(stream, dptr);
            Tij.emplace_back(t);
          } else {
            auto t = new DenseTile<scalar_t>(m, n);
            // this could be copied straight to the GPU
            std::copy(ptr, ptr+m*n, t->D().data());  ptr += m*n;
            t->move_to_gpu(stream, dptr);
            Tij.emplace_back(t);
          }
        }
      return Tij;
    }

    template<typename scalar_t>
    std::vector<std::unique_ptr<BLRTile<scalar_t>>>
    BLRMatrixMPI<scalar_t>::bcast_col_of_tiles_along_rows_gpu
    (std::size_t i0, std::size_t i1, std::size_t j,
     gpu::Stream& stream, scalar_t* dptr, scalar_t* pinned) const {
      if (!grid()) return {};
      int src = j % grid()->npcols();
      std::size_t msg_size = 0, nr_tiles = 0;
      std::vector<std::int64_t> ranks;
      if (grid()->is_local_col(j)) {
        for (std::size_t i=i0; i<i1; i++)
          if (grid()->is_local_row(i)) {
            msg_size += tile(i, j).nonzeros();
            ranks.push_back(tile(i, j).is_low_rank() ?
                            tile(i, j).rank() : -1);
            nr_tiles++;
          }
      } else {
        for (std::size_t i=i0; i<i1; i++)
          if (grid()->is_local_row(i))
            nr_tiles++;
        ranks.resize(nr_tiles);
      }
      std::vector<std::unique_ptr<BLRTile<scalar_t>>> Tij;
      if (ranks.empty()) return Tij;
      ranks.push_back(msg_size);
      grid()->row_comm().broadcast_from(ranks, src);
      msg_size = ranks.back();
      std::vector<scalar_t> buf(msg_size);
      auto ptr = buf.data();
      if (grid()->is_local_col(j)) {
        for (std::size_t i=i0; i<i1; i++)
          if (grid()->is_local_row(i)) {
            auto& t = tile(i, j);
            if (t.is_low_rank()) {
              gpu_check(gpu::copy_device_to_host
                        (ptr, t.U().data(), t.U().nonzeros()));
              ptr += t.U().nonzeros();
              gpu_check(gpu::copy_device_to_host
                        (ptr, t.V().data(), t.V().nonzeros()));
              ptr += t.V().nonzeros();
            } else {
              gpu_check(gpu::copy_device_to_host
                        (ptr, t.D().data(), t.D().nonzeros()));
              ptr += t.D().nonzeros();
            }
          }
      }
      // TODO CUDA aware MPI
      grid()->row_comm().broadcast_from(buf, src);
      Tij.reserve(nr_tiles);
      ptr = buf.data();
      auto n = tilecols(j);
      for (std::size_t i=i0; i<i1; i++)
        if (grid()->is_local_row(i)) {
          auto r = ranks[Tij.size()];
          auto m = tilerows(i);
          if (r != -1) {
            auto t = new LRTile<scalar_t>(m, n, r);
            // this could be copied straight to the GPU
            std::copy(ptr, ptr+m*r, t->U().data());  ptr += m*r;
            std::copy(ptr, ptr+r*n, t->V().data());  ptr += r*n;
            t->move_to_gpu(stream, dptr);
            Tij.emplace_back(t);
          } else {
            auto t = new DenseTile<scalar_t>(m, n);
            // this could be copied straight to the GPU
            std::copy(ptr, ptr+m*n, t->D().data());  ptr += m*n;
            t->move_to_gpu(stream, dptr);
            Tij.emplace_back(t);
          }
        }
      return Tij;
    }


    /*
     * Input matrices are on CPU.
     */
    template<typename scalar_t> std::vector<int>
    BLRMatrixMPI<scalar_t>::partial_factor_gpu
    (BLRMPI_t& A11, BLRMPI_t& A12, BLRMPI_t& A21, BLRMPI_t& A22,
     const adm_t& adm, const Opts_t& opts) {
      auto g = A11.grid();
      std::vector<int> piv, piv_tile;
      if (!g->active()) return piv;

      gpu::Stream copy_stream, comp_stream;
      gpu::BLASHandle handle(comp_stream);
      gpu::SOLVERHandle solve_handle(comp_stream),
        solve_handle2(copy_stream);

      auto rb = A11.rowblocks();
      auto rb2 = A22.rowblocks();
      int max_batchcount =
        A11.blocks_.size() + A12.blocks_.size() +
        A21.blocks_.size() + A22.blocks_.size();
      std::size_t max_m1 = 0, max_m2 = 0;
      for (std::size_t k=0; k<rb; k++)
        max_m1 = std::max(max_m1, A11.tilerows(k));
      for (std::size_t k=0; k<rb2; k++)
        max_m2 = std::max(max_m2, A22.tilerows(k));
      auto max_m = std::max(max_m1, max_m2);
      auto max_mn = max_m * max_m;

#if defined(STRUMPACK_USE_KBLAS)
      VBatchedARA<scalar_t>::kblas_wsquery(handle, 2*(rb+rb2-1));
#else
      // TODO no KBLAS
#endif

      VectorPool<scalar_t> workspace;

      auto d_batch_meta = VBatchedGEMM<scalar_t>::dwork_bytes(max_batchcount);
      gpu::DeviceMemory<char> d_batch_mem(3*d_batch_meta);

      gpu::HostMemory<scalar_t> pinned = workspace.get_pinned(max_mn);
      int getrf_work_size =
        gpu::getrf_buffersize<scalar_t>(solve_handle, max_m1);
      gpu::DeviceMemory<scalar_t> d_work_mem(getrf_work_size);
      gpu::DeviceMemory<scalar_t> dtemp_col
        (max_m*std::max(A11.lrows(), A21.lrows()));
      gpu::DeviceMemory<scalar_t> dtemp_row_1(max_m1*A11.lcols());
      gpu::DeviceMemory<scalar_t> dtemp_row_2(max_m1*A22.lcols());
      gpu::DeviceMemory<scalar_t> dtemp_col_1(max_m1*A11.lrows());
      gpu::DeviceMemory<scalar_t> dtemp_col_2(max_m1*A22.lrows());
      gpu::DeviceMemory<int> dpiv(max_m);
      gpu::DeviceMemory<int> dinfo(1);
      gpu::DeviceMemory<char> d_batch_matrix_mem;
      // TODO allocate dA22 later to reduce GPU peak memory?
      gpu::DeviceMemory<scalar_t>
        dA11(A11.lrows()*A11.lcols()), dA12(A12.lrows()*A12.lcols()),
        dA21(A21.lrows()*A12.lcols()), dA22(A22.lrows()*A22.lcols());

      // TODO do this column wise to overlap
      A11.move_to_gpu(copy_stream, dA11);
      A12.move_to_gpu(copy_stream, dA12);
      A21.move_to_gpu(copy_stream, dA21);
      A22.move_to_gpu(copy_stream, dA22);

      DenseTile<scalar_t> Tii;
      for (std::size_t i=0; i<rb; i++) {
        auto mi = A11.tilerows(i);
        if (g->is_local_row(i)) {
          piv_tile.resize(mi);
          if (g->is_local_col(i)) {
            gpu::getrf<scalar_t>(solve_handle2, A11.tile(i, i).D(),
                                 d_work_mem, dpiv, dinfo);
            gpu_check(gpu::copy_device_to_host<int>
                      (piv_tile.data(), dpiv, mi));
          }
          // TODO CUDA aware bcast?
          g->row_comm().broadcast_from(piv_tile, i % g->npcols());
          if (!g->is_local_col(i))
            gpu_check(gpu::copy_host_to_device<int>
                      (dpiv, piv_tile.data(), mi));
          int r0 = A11.tileroff(i);
          std::transform
            (piv_tile.begin(), piv_tile.end(), std::back_inserter(piv),
             [r0](int p) -> int { return p + r0; });

          Tii = A11.bcast_dense_tile_along_row_gpu
            (i, i, copy_stream, dtemp_col_1);
        }
        if (g->is_local_col(i))
          Tii = A11.bcast_dense_tile_along_col_gpu
            (i, i, copy_stream, dtemp_row_1);

#if defined(STRUMPACK_USE_KBLAS)
        VBatchedARA<scalar_t> ara;
        if (g->is_local_row(i)) {
          for (std::size_t j=i+1; j<rb; j++)
            if (g->is_local_col(j) && adm(i, j))
              ara.add(A11.block(i, j));
          for (std::size_t j=0; j<rb2; j++)
            if (g->is_local_col(j))
              ara.add(A12.block(i, j));
        }
        if (g->is_local_col(i)) {
          for (std::size_t j=i+1; j<rb; j++)
            if (g->is_local_row(j) && adm(j, i))
              ara.add(A11.block(j, i));
          for (std::size_t j=0; j<rb2; j++)
            if (g->is_local_row(j))
              ara.add(A21.block(j, i));
        }
        ara.run(handle, workspace, opts.rel_tol());
#else
        std::cout << "TODO BLR compression requires KBLAS for now" << std::endl;
#endif
        copy_stream.synchronize(); // this is the stream used for the
                                   // getrf

        VBatchedTRSM<scalar_t> batched_trsm_left, batched_trsm_right;
        if (g->is_local_row(i)) {
          for (std::size_t j=i+1; j<rb; j++)
            if (g->is_local_col(j)) {
              A11.tile(i, j).laswp(handle, dpiv, true);
              batched_trsm_left.add(Tii.D(), A11.tile(i, j).U());
            }
          for (std::size_t j=0; j<rb2; j++)
            if (g->is_local_col(j)) {
              A12.tile(i, j).laswp(handle, dpiv, true);
              batched_trsm_left.add(Tii.D(), A12.tile(i, j).U());
            }
        }
        if (g->is_local_col(i)) {
          for (std::size_t j=i+1; j<rb; j++)
            if (g->is_local_row(j))
              batched_trsm_right.add(Tii.D(), A11.tile(j, i).V());
          for (std::size_t j=0; j<rb2; j++)
            if (g->is_local_row(j))
              batched_trsm_right.add(Tii.D(), A21.tile(j, i).V());
        }
        batched_trsm_left.run(handle, workspace, true);
        batched_trsm_right.run(handle, workspace, false);

        // Schur complement update
        auto Tij = A11.bcast_row_of_tiles_along_cols_gpu
          (i, i+1, rb, copy_stream, dtemp_row_1);
        auto Tij2 = A12.bcast_row_of_tiles_along_cols_gpu
          (i, 0, rb2, copy_stream, dtemp_row_2);
        auto Tki = A11.bcast_col_of_tiles_along_rows_gpu
          (i+1, rb, i, copy_stream, dtemp_col_1);
        auto Tk2i = A21.bcast_col_of_tiles_along_rows_gpu
          (0, rb2, i, copy_stream, dtemp_col_2);

        int batchcount = 0;
        std::size_t sVU = 0, sUVU = 0;
        for (std::size_t k=i+1, lk=0; k<rb; k++) {
          if (!g->is_local_row(k)) continue;
          for (std::size_t j=i+1, lj=0; j<rb; j++)
            if (g->is_local_col(j)) {
              batchcount++;
              multiply_inc_work_size(*(Tki[lk]), *(Tij[lj++]), sVU, sUVU);
            }
          lk++;
        }
        for (std::size_t k=0, lk=0; k<rb2; k++) {
          if (!g->is_local_row(k)) continue;
          for (std::size_t j=i+1, lj=0; j<rb; j++)
            if (g->is_local_col(j)) {
              multiply_inc_work_size(*(Tk2i[lk]), *(Tij[lj++]), sVU, sUVU);
              batchcount++;
            }
          lk++;
        }
        for (std::size_t k=i+1, lk=0; k<rb; k++) {
          if (!g->is_local_row(k)) continue;
          for (std::size_t j=0, lj=0; j<rb2; j++)
            if (g->is_local_col(j)) {
              batchcount++;
              multiply_inc_work_size(*(Tki[lk]), *(Tij2[lj++]), sVU, sUVU);
            }
          lk++;
        }
        for (std::size_t k=0, lk=0; k<rb2; k++) {
          if (!g->is_local_row(k)) continue;
          for (std::size_t j=0, lj=0; j<rb2; j++)
            if (g->is_local_col(j)) {
              batchcount++;
              multiply_inc_work_size(*(Tk2i[lk]), *(Tij2[lj++]), sVU, sUVU);
            }
          lk++;
        }

        assert(batchcount <= max_batchcount);
        if ((sVU+sUVU)*sizeof(scalar_t) > d_batch_matrix_mem.size()) {
          workspace.restore(d_batch_matrix_mem);
          d_batch_matrix_mem = workspace.get_device_bytes
            ((sVU+sUVU)*sizeof(scalar_t));
        }
        auto dVU = d_batch_matrix_mem.template as<scalar_t>();
        auto dUVU = dVU + sVU;
        VBatchedGEMM<scalar_t> b1(batchcount, d_batch_mem),
          b2(batchcount, d_batch_mem+gpu::round_up(d_batch_meta)),
          b3(batchcount, d_batch_mem+2*gpu::round_up(d_batch_meta));

        for (std::size_t k=i+1, lk=0; k<rb; k++) {
          if (!g->is_local_row(k)) continue;
          for (std::size_t j=i+1, lj=0; j<rb; j++)
            if (g->is_local_col(j))
              add_tile_mult
                (*(Tki[lk]), *(Tij[lj++]), A11.tile_dense(k, j).D(),
                 b1, b2, b3, dVU, dUVU);
          lk++;
        }
        for (std::size_t k=0, lk=0; k<rb2; k++) {
          if (!g->is_local_row(k)) continue;
          for (std::size_t j=i+1, lj=0; j<rb; j++)
            if (g->is_local_col(j))
              add_tile_mult
                (*(Tk2i[lk]), *(Tij[lj++]), A21.tile_dense(k, j).D(),
                 b1, b2, b3, dVU, dUVU);
          lk++;
        }
        for (std::size_t k=i+1, lk=0; k<rb; k++) {
          if (!g->is_local_row(k)) continue;
          for (std::size_t j=0, lj=0; j<rb2; j++)
            if (g->is_local_col(j))
              add_tile_mult
                (*(Tki[lk]), *(Tij2[lj++]), A12.tile_dense(k, j).D(),
                 b1, b2, b3, dVU, dUVU);
          lk++;
        }
        for (std::size_t k=0, lk=0; k<rb2; k++) {
          if (!g->is_local_row(k)) continue;
          for (std::size_t j=0, lj=0; j<rb2; j++)
            if (g->is_local_col(j))
              add_tile_mult
                (*(Tk2i[lk]), *(Tij2[lj++]), A22.tile_dense(k, j).D(),
                 b1, b2, b3, dVU, dUVU);
          lk++;
        }

        b1.run(scalar_t(1.), scalar_t(0.), comp_stream, handle);
        b2.run(scalar_t(1.), scalar_t(0.), comp_stream, handle);
        b3.run(scalar_t(-1.), scalar_t(1.), comp_stream, handle);
      }
      A11.move_to_cpu(copy_stream, pinned);
      A12.move_to_cpu(copy_stream, pinned);
      A21.move_to_cpu(copy_stream, pinned);
      A22.move_to_cpu(copy_stream, pinned);
      return piv;
    }

    // explicit template instantiations
    template class BLRMatrixMPI<float>;
    template class BLRMatrixMPI<double>;
    template class BLRMatrixMPI<std::complex<float>>;
    template class BLRMatrixMPI<std::complex<double>>;

  } // end namespace BLR
} // end namespace strumpack
