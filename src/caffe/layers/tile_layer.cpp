#include <vector>

#include "caffe/layers/tile_layer.hpp"
#include "caffe/util/math_functions.hpp"

namespace caffe {

template <typename Dtype>
void TileLayer<Dtype>::Reshape(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  const TileParameter& tile_param = this->layer_param_.tile_param();
  axis_ = bottom[0]->CanonicalAxisIndex(tile_param.axis());
  CHECK(tile_param.has_tiles()) << "Number of tiles must be specified";
  tiles_ = tile_param.tiles();
  CHECK_GT(tiles_, 0) << "Number of tiles must be positive.";
  vector<int> top_shape = bottom[0]->shape();
  top_shape[axis_] = bottom[0]->shape(axis_) * tiles_;
  top[0]->Reshape(top_shape);
  outer_dim_ = bottom[0]->count(0, axis_);
  inner_dim_ = bottom[0]->count(axis_);
}

template <typename Dtype>
void TileLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->cpu_data();
  Dtype* top_data = top[0]->mutable_cpu_data();
  for (int i = 0; i < outer_dim_; ++i) {
    for (int t = 0; t < tiles_; ++t) {
      caffe_copy(inner_dim_, bottom_data, top_data);
      top_data += inner_dim_;
    }
    bottom_data += inner_dim_;
  }
}

#ifdef CPU_ONLY
STUB_GPU(TileLayer);
#elif USE_OPENCL



template <typename Dtype>
void TileLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {

 
  const Dtype* bottom_data = bottom[0]->gpu_data();
  Dtype* top_data = top[0]->mutable_gpu_data();
  const int bottom_tile_axis = bottom[0]->shape(axis_);
  const int nthreads = top[0]->count();


  cl_int ret;

  cl_kernel kernel = clCreateKernel(Caffe::Get().math_program, "Tile", &ret);
  OPENCL_CHECK(ret);

  // Set arguments for kernel
  OPENCL_CHECK(clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *)&bottom_data));  
  OPENCL_CHECK(clSetKernelArg(kernel, 1, sizeof(cl_mem), (void *)&top_data));  
  OPENCL_CHECK(clSetKernelArg(kernel, 2, sizeof(cl_int), (void *)&inner_dim_));
  OPENCL_CHECK(clSetKernelArg(kernel, 3, sizeof(cl_int), (void *)&tiles_));
  OPENCL_CHECK(clSetKernelArg(kernel, 4, sizeof(cl_int), (void *)&bottom_tile_axis));
  OPENCL_CHECK(clSetKernelArg(kernel, 5, sizeof(cl_int), (void *)&nthreads));

  size_t global_size = CAFFE_GET_BLOCKS(nthreads);
  
  OPENCL_CHECK(clEnqueueNDRangeKernel(Caffe::Get().commandQueue, kernel, 1, NULL, &global_size, &CAFFE_CUDA_NUM_THREADS, 0, NULL, NULL));  


}


#endif

INSTANTIATE_CLASS(TileLayer);
REGISTER_LAYER_CLASS(Tile);

}  // namespace caffe
