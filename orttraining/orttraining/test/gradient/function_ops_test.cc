// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "gtest/gtest.h"

#include "core/graph/contrib_ops/contrib_defs.h"
#include "orttraining/core/graph/training_op_defs.h"

#include "test/contrib_ops/function_test_util.h"

using namespace ::onnxruntime::common;

namespace onnxruntime {
namespace test {

static void RegisterSchemas() {
  static bool registered = false;
  if (!registered) {
    onnxruntime::training::RegisterTrainingOpSchemas();
    registered = true;
  }
}

class FunExpansionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterSchemas();
  }
};

static void
InitSoftmaxGradTestCase(FunctionTestCase& testCase, std::vector<int64_t> shape) {
  testCase.AddInput<float>("dY", shape);
  testCase.AddInput<float>("Y", shape);
  testCase.AddOutput("dX");
}

TEST_F(FunExpansionTest, SoftmaxGrad_DefaultAxis) {
  FunctionTestCase testCase("SoftmaxGrad");
  InitSoftmaxGradTestCase(testCase, {3, 2});
  testCase.RunTest();
}

TEST_F(FunExpansionTest, SoftmaxGrad_NegativeAxis) {
  FunctionTestCase testCase("SoftmaxGrad");
  InitSoftmaxGradTestCase(testCase, {3, 2});
  testCase.AddAttribute("axis", -1);
  testCase.RunTest();
}

TEST_F(FunExpansionTest, SoftmaxGrad_PositiveAxis) {
  FunctionTestCase testCase("SoftmaxGrad");
  InitSoftmaxGradTestCase(testCase, {3, 2});
  testCase.AddAttribute("axis", 1);
  testCase.RunTest();
}

TEST_F(FunExpansionTest, SoftmaxGrad_3D) {
  FunctionTestCase testCase("SoftmaxGrad");
  InitSoftmaxGradTestCase(testCase, {3, 2, 2});
  testCase.RunTest();
}

TEST_F(FunExpansionTest, SoftmaxGrad_SymbolicShape) {
  FunctionTestCase testCase("SoftmaxGrad");
  std::vector<int64_t> shape{3, 2, 2};
  std::vector<std::string> sym_shape{"BatchSize", "SeqSize", "2"};
  int size = 12;
  std::vector<float> value(size);
  for (int64_t i = 0; i < size; i++)
    value[i] = float(i);

  testCase.AddInput("dY", shape, value, sym_shape);
  testCase.AddInput("Y", shape, value, sym_shape);
  testCase.AddOutput("dX");
  testCase.RunTest();
}

// Test (unexpanded) versions for both opset 12 and opset 13 models to ensure
// function-schema does not impact handling of opset 12 models. The current
// expansion requires opset 13, and no expansion should happen in opset 12
// models. Test is required since ORT currently generates function-expansion
// even when op is dispatched to a kernel.

TEST_F(FunExpansionTest, SoftmaxGrad_OpsetTest) {
  FunctionTestCase testCase("SoftmaxGrad");
  testCase.opsets[kOnnxDomain] = 12;
  testCase.opsets[kMSDomain] = 1;
  InitSoftmaxGradTestCase(testCase, {3, 2, 2});

  auto model1 = testCase.CreateModel();
  auto results1 = FunctionTestCase::Run(*model1, testCase.input_value_map, testCase.output_names);

  testCase.opsets[kOnnxDomain] = 13;
  testCase.opsets[kMSDomain] = 1;

  auto model2 = testCase.CreateModel();
  auto results2 = FunctionTestCase::Run(*model1, testCase.input_value_map, testCase.output_names);

  FunctionTestCase::AssertEqual(results1, results2);
}

template <typename T>
void DropoutGradWithoutRatio() {
  FunctionTestCase testCase("DropoutGrad");
  std::vector<int64_t> shape{16, 4, 4};
  testCase.AddInput<T>("dY", shape);
  testCase.AddInput<bool>("mask", shape);
  testCase.AddOutput("dX");
  testCase.RunTest();
}

TEST_F(FunExpansionTest, DropoutGrad_WithoutRatio) {
  DropoutGradWithoutRatio<float>();
  DropoutGradWithoutRatio<double>();
}

template <typename T>
void DropoutGradWithRatio() {
  FunctionTestCase testCase("DropoutGrad");
  std::vector<int64_t> shape{16, 4, 4};
  testCase.AddInput<T>("dY", shape);
  testCase.AddInput<bool>("mask", shape);
  testCase.AddInput("ratio", {}, {0.5f});
  testCase.AddOutput("dX");
  testCase.RunTest();
}

TEST_F(FunExpansionTest, DropoutGrad_WithRatio) {
  DropoutGradWithRatio<float>();
  DropoutGradWithRatio<double>();
}

template <typename T>
void CheckDropoutGradWithoutRatio(bool inline_call) {
  FunctionTestCase testCase("DropoutGrad");
  std::vector<int64_t> shape{16, 4, 4};
  testCase.AddInput<T, false>("dY", shape);
  testCase.AddInput<bool, false>("mask", shape);
  testCase.AddOutput("dX");
  auto model = testCase.CreateModel(inline_call);
  if (!inline_call) {
    auto& node = *model->MainGraph().Nodes().begin();
    auto* fnbody = node.GetMutableFunctionBody(true);
    EXPECT_EQ(fnbody, nullptr);
  }
}

TEST_F(FunExpansionTest, DropoutGrad_WithoutRatio2) {
  // bfloat16 not yet supported by ONNX op Where
  CheckDropoutGradWithoutRatio<BFloat16>(false);
  CheckDropoutGradWithoutRatio<MLFloat16>(true);
}

template <typename T>
void CheckDropoutGradWithRatio(bool inline_call) {
  FunctionTestCase testCase("DropoutGrad");
  std::vector<int64_t> shape{16, 4, 4};
  testCase.AddInput<T, false>("dY", shape);
  testCase.AddInput<bool, false>("mask", shape);
  testCase.AddInput("ratio", {}, {0.5f});
  testCase.AddOutput("dX");
  testCase.CreateModel(inline_call);
}

TEST_F(FunExpansionTest, DropoutGrad_WithRatio2) {
  // bfloat16 not yet supported by ONNX op Where
  CheckDropoutGradWithRatio<BFloat16>(false);
  CheckDropoutGradWithRatio<MLFloat16>(true);
}

template <typename T, bool RunTest = true>
void TestUnaryOpGrad(const char* opname) {
  FunctionTestCase testCase(opname);
  std::vector<int64_t> shape{16, 4};
  testCase.AddInput<T, RunTest>("dY", shape);
  testCase.AddInput<T, RunTest>("X", shape);
  testCase.AddOutput("dX");
  if (RunTest)
    testCase.RunTest();
  else
    // Test only expanded model creation and model checking.
    testCase.CreateModel(true);
}

TEST_F(FunExpansionTest, GeluGrad_float) {
  TestUnaryOpGrad<float, true>("GeluGrad");
}

TEST_F(FunExpansionTest, GeluGrad_HalfPrecision) {
  TestUnaryOpGrad<BFloat16, false>("GeluGrad");
  TestUnaryOpGrad<MLFloat16, false>("GeluGrad");
}

TEST_F(FunExpansionTest, FastGeluGrad) {
  TestUnaryOpGrad<float, true>("FastGeluGrad");
  TestUnaryOpGrad<BFloat16, false>("FastGeluGrad");
  TestUnaryOpGrad<MLFloat16, false>("FastGeluGrad");
}

template <typename T, typename U, bool RunTest = true>
void TestLayerNormGrad(std::vector<int64_t> prefix_shape, std::vector<int64_t> suffix_shape) {
  FunctionTestCase testCase("LayerNormalizationGrad");
  std::vector<int64_t> input_shape(prefix_shape);
  for (auto d : suffix_shape)
    input_shape.push_back(d);
  std::vector<int64_t> stats_shape(prefix_shape);
  for (auto d : suffix_shape) {
    (void)d;
    stats_shape.push_back(1);
  }
  testCase.AddInput<T, RunTest>("Y_grad", input_shape);
  testCase.AddInput<T, RunTest>("X", input_shape);
  testCase.AddInput<T, RunTest>("scale", suffix_shape);
  testCase.AddInput<U, RunTest>("mean", stats_shape);
  testCase.AddInput<U, RunTest>("inv_std_dev", stats_shape);
  testCase.AddOutput("X_grad");
  testCase.AddOutput("scale_grad");
  testCase.AddOutput("bias_grad");
  testCase.AddAttribute("axis", prefix_shape.size());
  if (RunTest)
    testCase.RunTest();
  else
    // Test only expanded model creation and model checking.
    testCase.CreateModel(true);
}

TEST_F(FunExpansionTest, LayerNormalizationGrad) {
  TestLayerNormGrad<float, float, true>({4, 1}, {8, 4});
  TestLayerNormGrad<float, float, true>({}, {8, 4});
  TestLayerNormGrad<BFloat16, float, false>({}, {8, 4});
}

TEST_F(FunExpansionTest, SigmoidGrad_float) {
  TestUnaryOpGrad<float, true>("SigmoidGrad");
}

}  // namespace test
}  // namespace onnxruntime
