"""Generate a trivial ONNX identity model for testing OnnxInference wrapper.

Usage:
    python scripts/generate_identity_model.py

Output:
    tests/models/identity.onnx
"""

import os

try:
    import onnx
    from onnx import helper, TensorProto
except ImportError:
    print("Installing onnx...")
    os.system("pip install onnx")
    import onnx
    from onnx import helper, TensorProto

def main():
    # Input: float tensor [1, 512]
    X = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 512])
    Y = helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 512])

    # Single Identity node
    identity_node = helper.make_node("Identity", inputs=["input"], outputs=["output"])

    graph = helper.make_graph([identity_node], "identity_graph", [X], [Y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8

    onnx.checker.check_model(model)

    out_dir = os.path.join(os.path.dirname(__file__), "..", "tests", "models")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "identity.onnx")
    onnx.save(model, out_path)
    print(f"Saved: {out_path}")

if __name__ == "__main__":
    main()
