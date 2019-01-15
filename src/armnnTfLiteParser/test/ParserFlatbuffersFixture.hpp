//
// Copyright © 2017 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include "Schema.hpp"
#include <boost/filesystem.hpp>
#include <boost/assert.hpp>
#include <boost/format.hpp>
#include <experimental/filesystem>
#include <armnn/IRuntime.hpp>
#include <armnn/TypesUtils.hpp>
#include "test/TensorHelpers.hpp"

#include "armnnTfLiteParser/ITfLiteParser.hpp"

#include <backendsCommon/BackendRegistry.hpp>

#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

#include <schema_generated.h>
#include <iostream>

using armnnTfLiteParser::ITfLiteParser;
using TensorRawPtr = const tflite::TensorT *;

struct ParserFlatbuffersFixture
{
    ParserFlatbuffersFixture() :
        m_Parser(ITfLiteParser::Create()),
        m_Runtime(armnn::IRuntime::Create(armnn::IRuntime::CreationOptions())),
        m_NetworkIdentifier(-1)
    {
    }

    std::vector<uint8_t> m_GraphBinary;
    std::string m_JsonString;
    std::unique_ptr<ITfLiteParser, void (*)(ITfLiteParser *parser)> m_Parser;
    armnn::IRuntimePtr m_Runtime;
    armnn::NetworkId m_NetworkIdentifier;

    /// If the single-input-single-output overload of Setup() is called, these will store the input and output name
    /// so they don't need to be passed to the single-input-single-output overload of RunTest().
    std::string m_SingleInputName;
    std::string m_SingleOutputName;

    void Setup()
    {
        bool ok = ReadStringToBinary();
        if (!ok) {
            throw armnn::Exception("LoadNetwork failed while reading binary input");
        }

        armnn::INetworkPtr network =
                m_Parser->CreateNetworkFromBinary(m_GraphBinary);

        if (!network) {
            throw armnn::Exception("The parser failed to create an ArmNN network");
        }

        auto optimized = Optimize(*network, { armnn::Compute::CpuRef },
                                  m_Runtime->GetDeviceSpec());
        std::string errorMessage;

        armnn::Status ret = m_Runtime->LoadNetwork(m_NetworkIdentifier, move(optimized), errorMessage);

        if (ret != armnn::Status::Success)
        {
            throw armnn::Exception(
                boost::str(
                    boost::format("The runtime failed to load the network. "
                                    "Error was: %1%. in %2% [%3%:%4%]") %
                    errorMessage %
                    __func__ %
                    __FILE__ %
                    __LINE__));
        }
    }

    void SetupSingleInputSingleOutput(const std::string& inputName, const std::string& outputName)
    {
        // Store the input and output name so they don't need to be passed to the single-input-single-output RunTest().
        m_SingleInputName = inputName;
        m_SingleOutputName = outputName;
        Setup();
    }

    bool ReadStringToBinary()
    {
        std::string schemafile(&tflite_schema_start, &tflite_schema_end);

        // parse schema first, so we can use it to parse the data after
        flatbuffers::Parser parser;

        bool ok = parser.Parse(schemafile.c_str());
        BOOST_ASSERT_MSG(ok, "Failed to parse schema file");

        ok &= parser.Parse(m_JsonString.c_str());
        BOOST_ASSERT_MSG(ok, "Failed to parse json input");

        if (!ok)
        {
            return false;
        }

        {
            const uint8_t * bufferPtr = parser.builder_.GetBufferPointer();
            size_t size = static_cast<size_t>(parser.builder_.GetSize());
            m_GraphBinary.assign(bufferPtr, bufferPtr+size);
        }
        return ok;
    }

    /// Executes the network with the given input tensor and checks the result against the given output tensor.
    /// This overload assumes the network has a single input and a single output.
    template <std::size_t NumOutputDimensions, typename DataType>
    void RunTest(size_t subgraphId,
         const std::vector<DataType>& inputData,
         const std::vector<DataType>& expectedOutputData);

    /// Executes the network with the given input tensors and checks the results against the given output tensors.
    /// This overload supports multiple inputs and multiple outputs, identified by name.
    template <std::size_t NumOutputDimensions, typename DataType>
    void RunTest(size_t subgraphId,
                 const std::map<std::string, std::vector<DataType>>& inputData,
                 const std::map<std::string, std::vector<DataType>>& expectedOutputData);

    void CheckTensors(const TensorRawPtr& tensors, size_t shapeSize, const std::vector<int32_t>& shape,
                      tflite::TensorType tensorType, uint32_t buffer, const std::string& name,
                      const std::vector<float>& min, const std::vector<float>& max,
                      const std::vector<float>& scale, const std::vector<int64_t>& zeroPoint)
    {
        BOOST_CHECK(tensors);
        BOOST_CHECK_EQUAL(shapeSize, tensors->shape.size());
        BOOST_CHECK_EQUAL_COLLECTIONS(shape.begin(), shape.end(), tensors->shape.begin(), tensors->shape.end());
        BOOST_CHECK_EQUAL(tensorType, tensors->type);
        BOOST_CHECK_EQUAL(buffer, tensors->buffer);
        BOOST_CHECK_EQUAL(name, tensors->name);
        BOOST_CHECK(tensors->quantization);
        BOOST_CHECK_EQUAL_COLLECTIONS(min.begin(), min.end(), tensors->quantization.get()->min.begin(),
                                      tensors->quantization.get()->min.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(max.begin(), max.end(), tensors->quantization.get()->max.begin(),
                                      tensors->quantization.get()->max.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(scale.begin(), scale.end(), tensors->quantization.get()->scale.begin(),
                                      tensors->quantization.get()->scale.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(zeroPoint.begin(), zeroPoint.end(),
                                      tensors->quantization.get()->zero_point.begin(),
                                      tensors->quantization.get()->zero_point.end());
    }
};

template <std::size_t NumOutputDimensions, typename DataType>
void ParserFlatbuffersFixture::RunTest(size_t subgraphId,
                                       const std::vector<DataType>& inputData,
                                       const std::vector<DataType>& expectedOutputData)
{
    RunTest<NumOutputDimensions, DataType>(subgraphId,
                                           { { m_SingleInputName, inputData } },
                                           { { m_SingleOutputName, expectedOutputData } });
}

template <std::size_t NumOutputDimensions, typename DataType>
void
ParserFlatbuffersFixture::RunTest(size_t subgraphId,
                                  const std::map<std::string, std::vector<DataType>>& inputData,
                                  const std::map<std::string, std::vector<DataType>>& expectedOutputData)
{
    using BindingPointInfo = std::pair<armnn::LayerBindingId, armnn::TensorInfo>;

    // Setup the armnn input tensors from the given vectors.
    armnn::InputTensors inputTensors;
    for (auto&& it : inputData)
    {
        BindingPointInfo bindingInfo = m_Parser->GetNetworkInputBindingInfo(subgraphId, it.first);
        armnn::VerifyTensorInfoDataType<DataType>(bindingInfo.second);
        inputTensors.push_back({ bindingInfo.first, armnn::ConstTensor(bindingInfo.second, it.second.data()) });
    }

    // Allocate storage for the output tensors to be written to and setup the armnn output tensors.
    std::map<std::string, boost::multi_array<DataType, NumOutputDimensions>> outputStorage;
    armnn::OutputTensors outputTensors;
    for (auto&& it : expectedOutputData)
    {
        BindingPointInfo bindingInfo = m_Parser->GetNetworkOutputBindingInfo(subgraphId, it.first);
        armnn::VerifyTensorInfoDataType<DataType>(bindingInfo.second);
        outputStorage.emplace(it.first, MakeTensor<DataType, NumOutputDimensions>(bindingInfo.second));
        outputTensors.push_back(
                { bindingInfo.first, armnn::Tensor(bindingInfo.second, outputStorage.at(it.first).data()) });
    }

    m_Runtime->EnqueueWorkload(m_NetworkIdentifier, inputTensors, outputTensors);

    // Compare each output tensor to the expected values
    for (auto&& it : expectedOutputData)
    {
        BindingPointInfo bindingInfo = m_Parser->GetNetworkOutputBindingInfo(subgraphId, it.first);
        auto outputExpected = MakeTensor<DataType, NumOutputDimensions>(bindingInfo.second, it.second);
        BOOST_TEST(CompareTensors(outputExpected, outputStorage[it.first]));
    }
}
