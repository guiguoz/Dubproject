#pragma once

#ifdef SAXFX_HAS_ONNX

#include <onnxruntime_cxx_api.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// OnnxInference
//
// Lightweight wrapper around ONNX Runtime C++ API.
// Loads a .onnx model and runs synchronous inference on float tensors.
//
// Thread safety: one instance per thread (ORT session is not thread-safe
// when sharing the same Run call).  For the audio pipeline, use a dedicated
// inference thread + LockFreeQueue.
//
// Usage:
//   OnnxInference model("model.onnx");
//   std::vector<float> input = { ... };
//   auto output = model.run(input);
// ─────────────────────────────────────────────────────────────────────────────
class OnnxInference
{
public:
    /// Construct and load a model from the given .onnx file path.
    /// Throws std::runtime_error on failure.
    explicit OnnxInference(const std::string& modelPath)
        : env_(ORT_LOGGING_LEVEL_WARNING, "SaxFX")
    {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // ONNX Runtime on Windows expects wide strings for model paths
#ifdef _WIN32
        std::wstring wpath(modelPath.begin(), modelPath.end());
        session_ = std::make_unique<Ort::Session>(env_, wpath.c_str(), opts);
#else
        session_ = std::make_unique<Ort::Session>(env_, modelPath.c_str(), opts);
#endif

        cacheInputOutputNames();
        cacheInputShape();
    }

    /// Run inference with a flat float vector as input.
    /// Returns the first output tensor as a flat float vector.
    std::vector<float> run(const std::vector<float>& inputData)
    {
        // Build input tensor
        std::vector<int64_t> inputShape = inputShape_;
        if (inputShape.empty())
            inputShape = { 1, static_cast<int64_t>(inputData.size()) };

        // If the model has a dynamic second dimension, override it
        for (auto& dim : inputShape)
            if (dim <= 0) dim = static_cast<int64_t>(inputData.size());

        auto memory_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memory_info,
            const_cast<float*>(inputData.data()),
            inputData.size(),
            inputShape.data(),
            inputShape.size());

        // Run
        auto outputTensors = session_->Run(
            Ort::RunOptions{nullptr},
            inputNames_.data(),
            &inputTensor,
            1,
            outputNames_.data(),
            outputNames_.size());

        // Extract first output
        const float* outPtr = outputTensors[0].GetTensorData<float>();
        auto outInfo = outputTensors[0].GetTensorTypeAndShapeInfo();
        size_t outSize = static_cast<size_t>(outInfo.GetElementCount());

        return { outPtr, outPtr + outSize };
    }

    /// Number of elements expected by the first input tensor.
    /// Returns 0 if the shape is fully dynamic.
    size_t inputSize() const noexcept
    {
        int64_t total = 1;
        for (auto d : inputShape_)
        {
            if (d <= 0) return 0;
            total *= d;
        }
        return static_cast<size_t>(total);
    }

    bool isLoaded() const noexcept { return session_ != nullptr; }

private:
    void cacheInputOutputNames()
    {
        Ort::AllocatorWithDefaultOptions alloc;

        size_t numInputs = session_->GetInputCount();
        for (size_t i = 0; i < numInputs; ++i)
        {
            auto name = session_->GetInputNameAllocated(i, alloc);
            inputNamesOwned_.push_back(std::string(name.get()));
        }

        size_t numOutputs = session_->GetOutputCount();
        for (size_t i = 0; i < numOutputs; ++i)
        {
            auto name = session_->GetOutputNameAllocated(i, alloc);
            outputNamesOwned_.push_back(std::string(name.get()));
        }

        // Build raw char* arrays for ORT API
        for (auto& s : inputNamesOwned_)
            inputNames_.push_back(s.c_str());
        for (auto& s : outputNamesOwned_)
            outputNames_.push_back(s.c_str());
    }

    void cacheInputShape()
    {
        if (session_->GetInputCount() > 0)
        {
            auto typeInfo = session_->GetInputTypeInfo(0);
            auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            inputShape_ = tensorInfo.GetShape();
        }
    }

    Ort::Env                        env_;
    std::unique_ptr<Ort::Session>   session_;
    std::vector<std::string>        inputNamesOwned_;
    std::vector<std::string>        outputNamesOwned_;
    std::vector<const char*>        inputNames_;
    std::vector<const char*>        outputNames_;
    std::vector<int64_t>            inputShape_;
};

} // namespace dsp

#endif // SAXFX_HAS_ONNX
