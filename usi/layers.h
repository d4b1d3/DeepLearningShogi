﻿#pragma once

#include "cudnn_wrapper.h"

template<const int k, const int c, const int fsize, const int pad, const int stride = 1>
class ConvLayer {
public:
	ConvLayer() : W(nullptr), workSpace(nullptr) {
		const size_t size = c * k * fsize * fsize;
		checkCudaErrors(cudaMalloc((void**)&W, size * sizeof(DType)));
	}
	~ConvLayer() {
		checkCudaErrors(cudaFree(W));
		checkCudaErrors(cudaFree(workSpace));
	}

	void init(cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, cudnnTensorDescriptor_t yDesc) {
		checkCUDNN(cudnnSetFilter4dDescriptor(wDesc, CUDNN_DATA_TYPE, CUDNN_TENSOR_NCHW, k, c, fsize, fsize));
		checkCUDNN(cudnnSetConvolution2dDescriptor(convDesc, pad, pad, stride, stride, 1, 1, CUDNN_CROSS_CORRELATION, CUDNN_DATA_TYPE));
#ifdef FP16
		checkCUDNN(cudnnSetConvolutionMathType(convDesc, CUDNN_TENSOR_OP_MATH));
#else
		checkCUDNN(cudnnSetConvolutionMathType(convDesc, CUDNN_TENSOR_OP_MATH_ALLOW_CONVERSION));
#endif
		cudnnConvolutionFwdAlgoPerf_t algo_perf[4];
		int returnedAlgoCount;
		checkCUDNN(cudnnGetConvolutionForwardAlgorithm_v7(handle, xDesc, wDesc, convDesc, yDesc, 4, &returnedAlgoCount, algo_perf));
		int algo_index = 0;
#ifndef FP16
		for (int i = 0; i < returnedAlgoCount; i++) {
			if (algo_perf[i].algo == CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM && algo_perf[i].mathType == CUDNN_TENSOR_OP_MATH_ALLOW_CONVERSION) {
				algo_index = i;
				break;
			}
		}
#endif
		algo = algo_perf[algo_index].algo;
		workSpaceSizeInBytes = algo_perf[algo_index].memory;
		checkCudaErrors(cudaMalloc(&workSpace, workSpaceSizeInBytes));
	}

	int get_yh(const int h) {
		return (h + 2 * pad - fsize) / stride + 1;
	}

	int get_yw(const int w) {
		return (w + 2 * pad - fsize) / stride + 1;
	}

	void get_xdesc(cudnnTensorDescriptor_t xDesc, const int n, const int h, const int w) {
		checkCUDNN(cudnnSetTensor4dDescriptor(xDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_TYPE, n, c, h, w));
	}

	void get_ydesc(cudnnTensorDescriptor_t yDesc, const int n, const int h, const int w) {
		checkCUDNN(cudnnSetTensor4dDescriptor(yDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_TYPE, n, k, h, w));
	}

	int get_xsize(const int n, const int h, const int w) {
		return n * c * h * w * sizeof(DType);
	}

	int get_ysize(const int n, const int h, const int w) {
		return n * k * h * w * sizeof(DType);
	}

	void set_param(float* data) {
		const size_t size = c * k * fsize * fsize;
#ifdef FP16
		__half* tmp = new __half[size];
		for (size_t i = 0; i < size; i++)
			tmp[i] = __float2half(data[i]);
		checkCudaErrors(cudaMemcpy(W, tmp, size * sizeof(__half), cudaMemcpyHostToDevice));
		delete[] tmp;
#else
		checkCudaErrors(cudaMemcpy(W, data, size * sizeof(DType), cudaMemcpyHostToDevice));
#endif
	}

	void operator() (cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, DType* x, cudnnTensorDescriptor_t yDesc, DType* y) {
		const float alpha = 1.0f;
		const float beta = 0.0f;
		checkCUDNN(cudnnConvolutionForward(handle, &alpha, xDesc, x, wDesc, W, convDesc, algo, workSpace, workSpaceSizeInBytes, &beta, yDesc, y));
	}

private:
	CudnnFilterDescriptor wDesc;
	CudnnConvolutionDescriptor convDesc;
	cudnnConvolutionFwdAlgo_t algo;
	size_t workSpaceSizeInBytes;
	DType* W;
	void* workSpace;
};

template<const int c, const int h, const int w>
class Bias {
public:
	Bias() : b(nullptr) {
		checkCUDNN(cudnnSetTensor4dDescriptor(biasTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_TYPE, 1, c, h, w));
		const size_t size = c * h * w;
		checkCudaErrors(cudaMalloc((void**)&b, size * sizeof(DType)));
	}
	~Bias() {
		checkCudaErrors(cudaFree(b));
	}

	void set_bias(float* data) {
		const size_t size = c * h * w;
#ifdef FP16
		__half* tmp = new __half[size];
		for (size_t i = 0; i < size; i++)
			tmp[i] = __float2half(data[i]);
		checkCudaErrors(cudaMemcpy(b, tmp, size * sizeof(__half), cudaMemcpyHostToDevice));
		delete[] tmp;
#else
		checkCudaErrors(cudaMemcpy(b, data, size * sizeof(DType), cudaMemcpyHostToDevice));
#endif
	}

	void operator() (cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, DType* x) {
		const float alpha = 1.0f;
		const float beta = 1.0f;
		checkCUDNN(cudnnAddTensor(handle, &alpha, biasTensorDesc, b, &beta, xDesc, x));
	}

private:
	CudnnTensorDescriptor biasTensorDesc;
	DType *b;
};

class ReLU {
public:
	ReLU() {
		checkCUDNN(cudnnSetActivationDescriptor(activDesc, CUDNN_ACTIVATION_RELU, CUDNN_PROPAGATE_NAN, 0.0/*reluCeiling*/));
	}

	void operator() (cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, DType* x) {
		const float alpha = 1.0f;
		const float beta = 0.0f;
		checkCUDNN(cudnnActivationForward(handle, activDesc, &alpha, xDesc, x, &beta, xDesc, x));
	}

private:
	CudnnActivationDescriptor activDesc;
};

template<const int k, const int n>
class Linear {
public:
	Linear() : W(nullptr) {
		const size_t size = k * n;
		checkCudaErrors(cudaMalloc((void**)&W, size * sizeof(DType)));
	}
	~Linear() {
		checkCudaErrors(cudaFree(W));
	}

	void get_xdesc(cudnnTensorDescriptor_t xDesc, const int m) {
		checkCUDNN(cudnnSetTensor4dDescriptor(xDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_TYPE, m, k, 1, 1));
	}

	void get_ydesc(cudnnTensorDescriptor_t yDesc, const int m) {
		checkCUDNN(cudnnSetTensor4dDescriptor(yDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_TYPE, m, n, 1, 1));
	}

	void set_param(float* data) {
		const size_t size = k * n;
#ifdef FP16
		__half* tmp = new __half[size];
		for (size_t i = 0; i < size; i++)
			tmp[i] = __float2half(data[i]);
		checkCudaErrors(cudaMemcpy(W, tmp, size * sizeof(__half), cudaMemcpyHostToDevice));
		delete[] tmp;
#else
		checkCudaErrors(cudaMemcpy(W, data, size * sizeof(DType), cudaMemcpyHostToDevice));
#endif
	}

	void operator() (cublasHandle_t handle, const int m, DType* x, DType* y) {
		const DType alpha = _one;
		const DType beta = _zero;
		// C = α op ( A ) op ( B ) + β C
		// op ( A ) m × k , op ( B ) k × n and C m × n
		checkCublasErrors(cublasGemmEx(handle, CUBLAS_OP_T, CUBLAS_OP_N, n, m, k, &alpha, W, CUDA_DATA_TYPE, k, x, CUDA_DATA_TYPE, k, &beta, y, CUDA_DATA_TYPE, n, CUDA_DATA_TYPE, CUBLAS_GEMM_DEFAULT_TENSOR_OP));
	}

private:
	DType* W;
};

template<const int window, const int stride = window, const int pad = 0>
class MaxPooling2D {
public:
	MaxPooling2D() {
		checkCUDNN(cudnnSetPooling2dDescriptor(poolingDesc, CUDNN_POOLING_MAX, CUDNN_PROPAGATE_NAN, window, window, pad, pad, stride, stride));
	}

	int get_yh(const int h) {
		return (h + 2 * pad - window) / stride + 1;
	}

	int get_yw(const int w) {
		return (w + 2 * pad - window) / stride + 1;
	}

	void get_desc(cudnnTensorDescriptor_t desc, const int n, const int c, const int h, const int w) {
		checkCUDNN(cudnnSetTensor4dDescriptor(desc, CUDNN_TENSOR_NCHW, CUDNN_DATA_TYPE, n, c, h, w));
	}

	void operator() (cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, DType* x, cudnnTensorDescriptor_t yDesc, DType* y) {
		const float alpha = 1.0f;
		const float beta = 0.0f;
		checkCUDNN(cudnnPoolingForward(handle, poolingDesc, &alpha, xDesc, x, &beta, yDesc, y));
	}

private:
	CudnnPoolingDescriptor poolingDesc;
};

class Softmax {
public:
	void get_desc(cudnnTensorDescriptor_t desc, const int n, const int c) {
		checkCUDNN(cudnnSetTensor4dDescriptor(desc, CUDNN_TENSOR_NCHW, CUDNN_DATA_TYPE, n, c, 1, 1));
	}

	void operator() (cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, DType* x) {
		const float alpha = 1.0f;
		const float beta = 0.0f;
		checkCUDNN(cudnnSoftmaxForward(handle, CUDNN_SOFTMAX_ACCURATE, CUDNN_SOFTMAX_MODE_CHANNEL, &alpha, xDesc, x, &beta, xDesc, x));
	}
};

class Sigmoid {
public:
	Sigmoid() {
		checkCUDNN(cudnnSetActivationDescriptor(activDesc, CUDNN_ACTIVATION_SIGMOID, CUDNN_PROPAGATE_NAN, 0.0));
	}

	void operator() (cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, DType* x) {
		const float alpha = 1.0f;
		const float beta = 0.0f;
		checkCUDNN(cudnnActivationForward(handle, activDesc, &alpha, xDesc, x, &beta, xDesc, x));
	}

private:
	CudnnActivationDescriptor activDesc;
};

template<const int k>
class BatchNormalization {
public:
	BatchNormalization() : bnScale(nullptr), bnBias(nullptr), estimatedMean(nullptr), estimatedVariance(nullptr) {
		const size_t size = k;
		checkCudaErrors(cudaMalloc((void**)&bnScale, size * sizeof(float)));
		checkCudaErrors(cudaMalloc((void**)&bnBias, size * sizeof(float)));
		checkCudaErrors(cudaMalloc((void**)&estimatedMean, size * sizeof(float)));
		checkCudaErrors(cudaMalloc((void**)&estimatedVariance, size * sizeof(float)));
	}
	~BatchNormalization() {
		checkCudaErrors(cudaFree(bnScale));
		checkCudaErrors(cudaFree(bnBias));
		checkCudaErrors(cudaFree(estimatedMean));
		checkCudaErrors(cudaFree(estimatedVariance));
	}

	void operator() (cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, DType* x, DType* y) {
		const float alpha = 1.0f;
		const float beta = 0.0f;
		const double eps = 2e-5;
		checkCUDNN(cudnnDeriveBNTensorDescriptor(bnScaleBiasMeanVarDesc, xDesc, CUDNN_BATCHNORM_SPATIAL));
		checkCUDNN(cudnnBatchNormalizationForwardInference(handle, CUDNN_BATCHNORM_SPATIAL, &alpha, &beta, xDesc, x, xDesc, y, bnScaleBiasMeanVarDesc, bnScale, bnBias, estimatedMean, estimatedVariance, eps));
	}

	void set_param(float* bnScale, float *bnBias, float *estimatedMean, float *estimatedVariance) {
		const size_t size = k;
		checkCudaErrors(cudaMemcpy(this->bnScale, bnScale, size * sizeof(float), cudaMemcpyHostToDevice));
		checkCudaErrors(cudaMemcpy(this->bnBias, bnBias, size * sizeof(float), cudaMemcpyHostToDevice));
		checkCudaErrors(cudaMemcpy(this->estimatedMean, estimatedMean, size * sizeof(float), cudaMemcpyHostToDevice));
		checkCudaErrors(cudaMemcpy(this->estimatedVariance, estimatedVariance, size * sizeof(float), cudaMemcpyHostToDevice));
	}

private:
	CudnnTensorDescriptor bnScaleBiasMeanVarDesc;
	float *bnScale;
	float *bnBias;
	float *estimatedMean;
	float *estimatedVariance;
};

class Add {
public:
	void operator() (cudnnHandle_t handle, cudnnTensorDescriptor_t xDesc, DType* x, DType* y) {
		const float alpha = 1.0f;
		const float beta = 1.0f;
		checkCUDNN(cudnnAddTensor(handle, &alpha, xDesc, x, &beta, xDesc, y));
	}
};