#include <vector>

#include "caffe/layers/conv_layer.hpp"

namespace caffe {

template <typename Dtype>
void ConvolutionLayer<Dtype>::compute_output_shape() {
  const int* kernel_shape_data = this->kernel_shape_.cpu_data();
  const int* stride_data = this->stride_.cpu_data();
  const int* pad_data = this->pad_.cpu_data();
  const int* dilation_data = this->dilation_.cpu_data();
  this->output_shape_.clear();
  for (int i = 0; i < this->num_spatial_axes_; ++i) {
    // i + 1 to skip channel axis
    const int input_dim = this->input_shape(i + 1);
    const int kernel_extent = dilation_data[i] * (kernel_shape_data[i] - 1) + 1;
    const int output_dim = (input_dim + 2 * pad_data[i] - kernel_extent)
        / stride_data[i] + 1;
    this->output_shape_.push_back(output_dim);
  }
}

template <typename Dtype>
void ConvolutionLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {

  cl_int ret = -1;

  cl_kernel kernel = clCreateKernel(this->program, (this->layer_param_.name() + "_forward").c_str(), &ret);

  for (int i = 0; i < bottom.size(); ++i) {

    const Dtype* weight = this->blobs_[0]->gpu_data();
    const Dtype* bottom_data = bottom[i]->gpu_data();
    Dtype* top_data = top[i]->mutable_gpu_data();


    OPENCL_CHECK(clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *)&bottom_data));  
    OPENCL_CHECK(clSetKernelArg(kernel, 1, sizeof(cl_mem), (void *)&weight));  
    OPENCL_CHECK(clSetKernelArg(kernel, 2, sizeof(cl_mem), (void *)&top_data));

    if (this->bias_term_) {
      const Dtype* bias = this->blobs_[1]->gpu_data();
      OPENCL_CHECK(clSetKernelArg(kernel, 3, sizeof(cl_mem), (void *)&bias));
    }

    size_t* local_size = new size_t[3];
    local_size[0] = static_cast<size_t>(16);
    local_size[1] = static_cast<size_t>(16);
    local_size[2] = static_cast<size_t>(1);

    size_t* global_size = new size_t[3];
    global_size[0] = static_cast<size_t>((((top[i]->shape(2) * top[i]->shape(3)) - 1) / 64 + 1)*16);
    global_size[1] = static_cast<size_t>((((top[i]->shape(1) / this->group_) - 1) / 64 + 1)*16);
    global_size[2] = static_cast<size_t>(bottom[i]->shape()[0] * 1);

    OPENCL_CHECK(clEnqueueNDRangeKernel(Caffe::Get().commandQueue, kernel, 3, NULL, global_size, local_size, 0, NULL, NULL));  

  }

}

template <typename Dtype>
void ConvolutionLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  const Dtype* weight = this->blobs_[0]->cpu_data();
  Dtype* weight_diff = this->blobs_[0]->mutable_cpu_diff();
  for (int i = 0; i < top.size(); ++i) {
    const Dtype* top_diff = top[i]->cpu_diff();
    const Dtype* bottom_data = bottom[i]->cpu_data();
    Dtype* bottom_diff = bottom[i]->mutable_cpu_diff();
    // Bias gradient, if necessary.
    if (this->bias_term_ && this->param_propagate_down_[1]) {
      Dtype* bias_diff = this->blobs_[1]->mutable_cpu_diff();
      for (int n = 0; n < this->num_; ++n) {
        this->backward_cpu_bias(bias_diff, top_diff + n * this->top_dim_);
      }
    }
    if (this->param_propagate_down_[0] || propagate_down[i]) {
      for (int n = 0; n < this->num_; ++n) {
        // gradient w.r.t. weight. Note that we will accumulate diffs.
        if (this->param_propagate_down_[0]) {
          this->weight_cpu_gemm(bottom_data + n * this->bottom_dim_,
              top_diff + n * this->top_dim_, weight_diff);
        }
        // gradient w.r.t. bottom data, if necessary.
        if (propagate_down[i]) {
          this->backward_cpu_gemm(top_diff + n * this->top_dim_, weight,
              bottom_diff + n * this->bottom_dim_);
        }
      }
    }
  }
}

template <typename Dtype>
void ConvolutionLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const Dtype* weight = this->blobs_[0]->cpu_data();
  for (int i = 0; i < bottom.size(); ++i) {
    const Dtype* bottom_data = bottom[i]->cpu_data();
    Dtype* top_data = top[i]->mutable_cpu_data();
    for (int n = 0; n < this->num_; ++n) {
      this->forward_cpu_gemm(bottom_data + n * this->bottom_dim_, weight,
          top_data + n * this->top_dim_);
      if (this->bias_term_) {
        const Dtype* bias = this->blobs_[1]->cpu_data();
        this->forward_cpu_bias(top_data + n * this->top_dim_, bias);
      }
    }
  }
}



template <typename Dtype>
void ConvolutionLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  const Dtype* weight = this->blobs_[0]->cpu_data();
  Dtype* weight_diff = this->blobs_[0]->mutable_cpu_diff();
  for (int i = 0; i < top.size(); ++i) {
    const Dtype* top_diff = top[i]->cpu_diff();
    const Dtype* bottom_data = bottom[i]->cpu_data();
    Dtype* bottom_diff = bottom[i]->mutable_cpu_diff();
    // Bias gradient, if necessary.
    if (this->bias_term_ && this->param_propagate_down_[1]) {
      Dtype* bias_diff = this->blobs_[1]->mutable_cpu_diff();
      for (int n = 0; n < this->num_; ++n) {
        this->backward_cpu_bias(bias_diff, top_diff + n * this->top_dim_);
      }
    }
    if (this->param_propagate_down_[0] || propagate_down[i]) {
      for (int n = 0; n < this->num_; ++n) {
        // gradient w.r.t. weight. Note that we will accumulate diffs.
        if (this->param_propagate_down_[0]) {
          this->weight_cpu_gemm(bottom_data + n * this->bottom_dim_,
              top_diff + n * this->top_dim_, weight_diff);
        }
        // gradient w.r.t. bottom data, if necessary.
        if (propagate_down[i]) {
          this->backward_cpu_gemm(top_diff + n * this->top_dim_, weight,
              bottom_diff + n * this->bottom_dim_);
        }
      }
    }
  }
}


template<typename Dtype>
std::string ConvolutionLayer<Dtype>::generate_fw_defs() {
  
  std::stringstream ss;

  int skip_bi = this->bottom_shape_[0].size() - this->output_shape_.size();
  int fmaps_in_ = this->bottom_shape_[0][skip_bi-1];
  int fmaps_out_ = this->num_output_;

  int B_off = fmaps_in_;
  int C_off = fmaps_out_;

  for (int i = 0; i < this->output_shape_.size(); ++i) {
    B_off *= this->bottom_shape_[0][skip_bi+i];
    C_off *= this->output_shape_[i];
  }


  // Input image batch offset
  this->add_def(ss, "v_B_off", B_off);
  // Output image batch offset
  this->add_def(ss, "v_C_off", C_off);

  int imsi = 1;
  int imso = 1;


  for (int i = 0; i < this->output_shape_.size(); ++i) {
    this->add_def(ss, "v_imsi_" + std::to_string(i), this->bottom_shape_[0][skip_bi+i]);
    imsi *= this->bottom_shape_[0][skip_bi+i];
    this->add_def(ss, "v_imso_" + std::to_string(i), this->output_shape_[i]);
    imso *= this->output_shape_[i];
  }
  this->add_def(ss, "v_imsi", imsi);
  this->add_def(ss, "v_imso", imso);

  for (int i = 0; i < this->kernel_shape_.count(); ++i) {
    this->add_def(ss, "v_k_" + std::to_string(i), this->kernel_shape_.cpu_data()[i]);
  }


  for (int i = 0; i < this->pad_.count(); ++i) {
    this->add_def(ss, "v_p_" + std::to_string(i), this->pad_.cpu_data()[i]);
  }

  for (int i = 0; i < this->stride_.count(); ++i) {
    this->add_def(ss, "v_s_" + std::to_string(i), this->stride_.cpu_data()[i]);
  }

  for (int i = 0; i < this->dilation_.count(); ++i) {
    this->add_def(ss, "v_d_" + std::to_string(i), this->dilation_.cpu_data()[i]);
  }

  this->add_def(ss, "v_fin", fmaps_in_);
  this->add_def(ss, "v_fout", fmaps_out_);

  // if (bias_term_) {
  //   this->add_def(ss, "v_bmul", bias_multiplier_);
  // }
  int MG_FW_ = fmaps_out_;
  int M_FW_ = fmaps_out_ / this->group_;
  int N_FW_ = 1;
  int KG_FW_ = fmaps_in_;
  int K_FW_ = fmaps_in_ / this->group_;

  for (int i = 0; i < this->output_shape_.size(); ++i) {
    K_FW_ *= this->kernel_shape_.cpu_data()[i];
    KG_FW_ *= this->kernel_shape_.cpu_data()[i];
    N_FW_ *= this->output_shape_[i];
  }

  // GEMM definitions
  this->add_def(ss, "MG", MG_FW_);
  this->add_def(ss, "M", M_FW_);
  this->add_def(ss, "N", N_FW_);
  this->add_def(ss, "KG", KG_FW_);
  this->add_def(ss, "K", K_FW_);

    // Local memory padding
  this->add_def(ss, "v_pad_A", 1);
  this->add_def(ss, "v_pad_B", 1);

  // The tile-size in dimension M
  this->add_def(ss, "TSM", 64);
  // The tile-size in dimension N
  this->add_def(ss, "TSN", 64);
  // The tile-size in dimension K
  this->add_def(ss, "TSK", 8);
  // TSK unrolling
  this->add_def(ss, "TSK_UNROLL", 1);
  // The work-per-thread in dimension M
  this->add_def(ss, "WPTM", 4);
  this->add_def(ss, "VWM", 4);
  // The work-per-thread in dimension N
  this->add_def(ss, "WPTN", 4);
  this->add_def(ss, "VWN", 4);
  // The reduced tile-size in dimension M
  this->add_def(ss, "RTSM", 16);
  // The reduced tile-size in dimension N
  this->add_def(ss, "RTSN", 16);
  // Loads-per-thread for A
  this->add_def(ss, "LPTA", "((TSK*TSM)/(RTSM*RTSN))");
  // Loads-per-thread for B
  this->add_def(ss, "LPTB", "((TSK*TSN)/(RTSM*RTSN))");

  // Num tiles needs to be next higher even integer
  // (due to some quirky bug in AMD OpenCL 2.0 on Windows)
  this->add_def(ss, "v_num_tiles", "(((K - 1)/(TSK*2) + 1)*2)");

  return ss.str();
}




template <typename Dtype>
std::string ConvolutionLayer<Dtype>::generate_fw_kernels(std::string name) {
  std::stringstream ss;

  int wptn = 4;
  int wptm = 4;
  int tsk = 8;
  int rtsn = 16;
  int rtsm = 16;
  int tsm = wptm * rtsm;
  int tsn = wptn * rtsn;
  int vwm = 4;
  int vwn = 4;
  int lpta = (tsm * tsk) / (rtsm * rtsn);
  int lptb = (tsn * tsk) / (rtsm * rtsn);

  bool skip_range_check_ = true;

  for (int i = 0; i < this->pad_.count(); ++i) {
    if (this->pad_.cpu_data()[i] > 0) {
      skip_range_check_ = false;
    }
  }

  // Forward kernel
  ss << "__kernel" << std::endl;
  ss << "__attribute__((reqd_work_group_size("
     << rtsn << ", " << rtsm << ", 1)))" << std::endl;
  ss << "__attribute__((vec_type_hint(Dtype"
     << std::min(vwm, vwn) << ")))" << std::endl;
  ss << "void " + name + "(";
  ss << "__global const Dtype* __restrict im_in, ";
  ss << "__global const Dtype* __restrict wg, ";
  ss << "__global Dtype* __restrict im_out";
  if (this->bias_term_) {
    ss << ", __global const Dtype* __restrict bias";
  }
  ss << ") {" << std::endl;

  // Thread identifiers
  // Local row ID (max: RTSM=TSM/WPTM)
  ss << "const int tidn = get_local_id(0);" << std::endl;
  // Local col ID (max: RTSN=TSN/WPTN)
  ss << "const int tidm = get_local_id(1);" << std::endl;
  // Work-group offset
  ss << "const int offN = TSN*get_group_id(0);" << std::endl;
  // Work-group offset
  ss << "const int offM = TSM*get_group_id(1);" << std::endl;

  // Local tile memory
  // Asub for loading weights & shuffling the output
  ss << "volatile __local Dtype Asub[" << tsm << "][" << tsk << " + v_pad_A];"
     << std::endl;
  // Bsub for loading the input image and shuffling the output image
  ss << "volatile __local Dtype Bsub[" << tsk << "][" << tsn << " + v_pad_B];"
     << std::endl;

  // Batch and group
  if (this->group_ > 1) {
    ss << "int group = get_global_id(2) % v_g;" << std::endl;
    ss << "int batch = get_global_id(2) / v_g;" << std::endl;
  } else {
    ss << "int batch = get_global_id(2);" << std::endl;
  }

  if (this->group_ > 1) {
    ss << "__global const Dtype* Aptr = wg + group * (M * K);" << std::endl;
    ss << "__global const Dtype* Bptr = im_in + v_B_off * batch "
       << "+ group * (v_B_off / v_g);" << std::endl;
    ss << "__global Dtype* Cptr = im_out + v_C_off * batch + group * (M * N);"
       << std::endl;
    if (this->bias_term_) {
      ss << "__global const Dtype* Dptr = bias + group * (v_fout / v_g);"
         << std::endl;
    }
  } else {
    ss << "__global const Dtype* Aptr = wg;" << std::endl;
    ss << "__global const Dtype* Bptr = im_in + v_B_off * batch;" << std::endl;
    ss << "__global Dtype* Cptr = im_out + v_C_off * batch;" << std::endl;
    if (this->bias_term_) {
      ss << "__global const Dtype* Dptr = bias;" << std::endl;
    }
  }

  // Initialize the accumulation registers
  ss << "{" << std::endl;  // Scoping for C registers
  ss << this->generate_accreg_init(false, false);

  ss << "{" << std::endl;  // Scoping for load & compute block
  // Loop over all tiles
  ss << "#pragma unroll 1" << std::endl;
  ss << "for (int t = 0; t < v_num_tiles; ++t) {" << std::endl;

  // Load one tile of A into local memory
  ss << "{" << std::endl;  // Scoping for loading A
  /*if (rtsn * rtsm % tsk == 0) {
    ss << "int tid = tidm * RTSN + tidn;" << std::endl;
    ss << "int row = tid / TSK;" << std::endl;
    ss << "int col = tid % TSK;" << std::endl;
    ss << "int tiledIndex = TSK * t + col;" << std::endl;
    int rowstep = (rtsn * rtsm) / tsk;
    for (int i = 0; i < lpta; ++i) {
      ss << "if ((offM + row + " << i * rowstep << ") < M && tiledIndex < K) {"
         << std::endl;
      ss << "Asub[row+" << i * rowstep << "][col] = Aptr[(offM + row + "
         << i * rowstep << ") * K + tiledIndex];" << std::endl;
      ss << "} else {" << std::endl;  // M-K-Guard
      ss << "Asub[row+" << i * rowstep << "][col] = 0.0;" << std::endl;
      ss << "}";
    }
  } else {*/
    ss << "#pragma unroll 4" << std::endl;
    ss << "for (int la = 0; la < LPTA; ++la) {" << std::endl;
    ss << "int tid = tidm * RTSN + tidn;" << std::endl;
    ss << "int id = la * RTSN * RTSM + tid;" << std::endl;
    ss << "int row = id / TSK;" << std::endl;
    ss << "int col = id % TSK;" << std::endl;
    ss << "int tiledIndex = TSK * t + col;" << std::endl;
    ss << "if ((offM + row) < M && tiledIndex < K) {" << std::endl;
    ss << "Asub[row][col] = Aptr[(offM + row) * K + tiledIndex];" << std::endl;
    ss << "} else {" << std::endl;  // M-K-Guard
    ss << "Asub[row][col] = 0.0;" << std::endl;
    ss << "}" << std::endl;
    ss << "}" << std::endl;  // LPTA
  //  }
  ss << "}" << std::endl;  // Scoping for loading A

  // Load one tile of B into local memory
  ss << "{" << std::endl;  // Scoping for loading B
  ss << "#pragma unroll 4" << std::endl;
  ss << "for (int lb = 0; lb < LPTB; ++lb) {" << std::endl;
  ss << "int tid = tidm * RTSN + tidn;" << std::endl;
  ss << "int id = lb * RTSN * RTSM + tid;" << std::endl;
  ss << "int col = id % TSN;" << std::endl;
  ss << "int row = id / TSN;" << std::endl;
  ss << "int tiledIndex = TSK * t + row;" << std::endl;

  ss << "if ((offN + col) < N && tiledIndex < K) {" << std::endl;
  // Define temporary registers
  for (int i = 0; i < this->num_spatial_axes_; ++i) {
    ss << "int d_iter_" << i << ";" << std::endl;
    ss << "int d_temp_" << i << ";" << std::endl;
  }

  ss << "int imageIndex = offN + col;" << std::endl;
  for (int i = this->num_spatial_axes_ - 1; i >= 0; --i) {
    // Compute d_iter, final tiledIndex becomes input feature map ID
    // Scale d_iter by the dilation factor
    ss << "d_iter_" << i << " = (tiledIndex % v_k_" << i << ") * v_d_" << i
       << ";" << std::endl;
    ss << "tiledIndex = tiledIndex / v_k_" << i << ";" << std::endl;

    // Compute d_temp
    // Scale d_temp by the stride and subtract the padding
    ss << "d_temp_" << i << " = (imageIndex % v_imso_" << i << ") * v_s_" << i
       << " - v_p_" << i << ";" << std::endl;
    ss << "imageIndex = imageIndex / v_imso_" << i << ";" << std::endl;
  }

  // Recombine final index, compute in-range
  if (!skip_range_check_) {
    ss << "bool in_range = true;" << std::endl;
  }
  ss << "int d_iter_im;" << std::endl;
  for (int i = 0; i < this->num_spatial_axes_; ++i) {
    // Here, d_temp_ represents the column shift,
    // while d_iter_ is the kernel shift
    ss << "d_iter_im = d_temp_" << i << " + d_iter_" << i << ";" << std::endl;
    ss << "tiledIndex = tiledIndex * v_imsi_" << i << " + d_iter_im;"
       << std::endl;
    if (!skip_range_check_) {
      ss << "in_range &= d_iter_im >= 0 && d_iter_im < v_imsi_" << i << ";"
         << std::endl;
    }
  }

  if (!skip_range_check_) {
    ss << "if (in_range) {" << std::endl;
  }
  // tiledIndex now holds the memory offset for the input image
  ss << "Bsub[row][col] = Bptr[tiledIndex];" << std::endl;
  if (!skip_range_check_) {
    ss << "} else {" << std::endl;
    ss << "Bsub[row][col] = 0.0;" << std::endl;
    ss << "}" << std::endl;
  }
  ss << "} else {" << std::endl;
  ss << "Bsub[row][col] = 0.0;" << std::endl;
  ss << "}" << std::endl;
  ss << "}" << std::endl;
  ss << "}" << std::endl;  // Scoping for loading B

  // Synchronize to make sure the tile is loaded
  ss << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

  ss << this->generate_gemm_core(false) << std::endl;

  // Synchronize before loading the next tile
  ss << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

  // Loop over all tiles
  ss << "}" << std::endl;
  ss << "}" << std::endl;  // Scoping for load & compute block


  // Store the final results in C
  /*ss << "#pragma unroll 1" << std::endl;
  ss << "for (int wn=0; wn<WPTN/VWN; ++wn) {" << std::endl;
  ss << "#pragma unroll" << std::endl;
  ss << "for (int wm=0; wm<WPTM/VWM; ++wm) {" << std::endl;
  for (int j = 0; j < vwn; ++j) {
    for (int i = 0; i < vwm; ++i) {
      ss << "Asub[(tidn+wn*RTSN)*VWN + " << j << "][(tidm + wn*RTSN)*VWM + " << i << "] = VEC_" << vwm << "_" << i << "(Creg[wn + " << j << "][wm]);" << std::endl;
    }
  }
  ss << "}" << std::endl;
  ss << "}" << std::endl;
  ss << "}" << std::endl;  // Scoping for C registers

  ss << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

  // Store the final results in C
  ss << "{" << std::endl; // Scoping for storing C
  ss << "Dtype" << vwm << " Creg;" << std::endl;
  ss << "#pragma unroll 1" << std::endl;
  ss << "for (int lc = 0; lc < ((TSM*TSN-1)/(RTSM*RTSN))/VWM+1; ++lc) {" << std::endl;
  ss << "int tid = tidm * RTSN + tidn;" << std::endl;
  ss << "int id = lc * RTSN * RTSM + tid;" << std::endl;
  ss << "int row = (id / TSN) * VWM;" << std::endl;
  ss << "int col = id % TSN;" << std::endl;
  ss << "int globalRow = offM + row;" << std::endl;
  ss << "int globalCol = offN + col;" << std::endl;
  for (int i = 0; i < vwm; ++i) {
    ss << "VEC_" << vwm << "_" << i << "(Creg) = Asub[col][row + " << i << "];" << std::endl;
    ss << "if ((globalRow +" << i << ") < M && globalCol < N) {" << std::endl;
    if (bias_term_) {
      ss << "Cptr[(globalRow +" << i << ") * N + globalCol] = VEC_" << vwm << "_" << i << "(Creg) + Dptr[globalRow +" << i << "];" << std::endl;
    } else {
      ss << "Cptr[(globalRow +" << i << ") * N + globalCol] = VEC_" << vwm << "_" << i << "(Creg);" << std::endl;
    }
    ss << "}" << std::endl;
  }
  ss << "}" << std::endl;
  ss << "}" << std::endl; // Scoping for storing C*/

  // Store the final results in C
  ss << "#pragma unroll" << std::endl;
  ss << "for (int wm=0; wm<WPTM; ++wm) {" << std::endl;
  ss << "int globalRow = offM + tidm + wm * RTSM;"
     << std::endl;
  if (this->bias_term_) {
    ss << "Dtype biasval = Dptr[globalRow];" << std::endl;
  }
  ss << "#pragma unroll" << std::endl;
  ss << "for (int wn=0; wn<WPTN; ++wn) {" << std::endl;
  ss << "int globalCol = offN + tidn + wn * RTSN;"
     << std::endl;
  ss << "if (globalRow < M && globalCol < N) {" << std::endl;
  if (this->bias_term_) {
    ss << "Cptr[globalRow * N + globalCol] = "
       << "((Dtype*)(&(Creg[wm][wn/VWN])))[wn%VWN] + biasval;"
       << std::endl;
  } else {
    ss << "Cptr[globalRow * N + globalCol] = "
       << "((Dtype*)(&(Creg[wm][wn/VWN])))[wn%VWN];" << std::endl;
  }
  ss << "}" << std::endl;   // M-N-Guard
  ss << "}" << std::endl;   // For (N)
  ss << "}" << std::endl;   // For (M)
  ss << "}" << std::endl;   // Scoping for C registers

  // Kernel
  ss << "}" << std::endl;

  return ss.str();
}





// #ifdef CPU_ONLY
// STUB_GPU(ConvolutionLayer);
// #endif

INSTANTIATE_CLASS(ConvolutionLayer);

}  // namespace caffe
