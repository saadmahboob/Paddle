/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

/*
 * This file defines the Node class and its subclasses. A Node is the basis
 * analysis element in a computation graph.
 * There are basically two kinds of nodes, the function node and value node.
 */
#pragma once

#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "paddle/fluid/framework/var_type.h"
#include "paddle/fluid/inference/analysis/device.h"
#include "paddle/fluid/inference/analysis/dot.h"
#include "paddle/fluid/inference/analysis/helper.h"
#include "paddle/fluid/platform/variant.h"

namespace paddle {
namespace inference {
namespace analysis {

class NodeMap;

// A helper class to maintain the status from Pass.
struct AnyAttr {
  using any_t =
      boost::variant<bool, float, int32_t, int64_t, void *, std::string>;
  // NOTE T should be a primary type or a struct combined by several primary
  // types.
  // NOTE the STL containers should not use here.
  // Some usages
  //   Attr attr;
  //   attr.Bool() = true;
  bool &Bool() { return As<bool>(); }
  float &Float() { return As<float>(); }
  int32_t &Int32() { return As<int32_t>(); }
  int64_t &Int64() { return As<int64_t>(); }
  void *&Pointer() { return As<void *>(); }
  std::string &String() { return As<std::string>(); }

  template <typename T>
  T &As() {
    if (type_index_ == typeid(AnyAttr)) {
      type_index_ = typeid(T);
      any_data_ = T();
    } else {
      PADDLE_ENFORCE(type_index_ == typeid(T), "fetch error type");
    }
    return boost::get<T>(any_data_);
  }

 private:
  any_t any_data_;
  std::type_index type_index_{typeid(AnyAttr)};
};

/*
 * Node Representation.
 *
 * This is a very important class for analysis. It is the base class of all
 * nodes computed by a program that may be used as operands to other nodes.
 * Node is the super class of other important classes such as Function and
 * Value, some nodes can have a name.
 */
class Node {
 public:
  // Node type. NOTE the new node types should add here.
  enum class Type { kNone = -1, kFunction, kValue, kFunctionBlock };

  Node() = default;

  // Cast to a subclass type, Function for example.
  template <typename Subclass>
  Subclass &As() {
    return *dynamic_cast<Subclass *>(this);
  }

  // Formatted representation of this Node.
  virtual std::string repr() const {
    return name() + "(" + std::to_string(id()) + ")";
  }

  // DOT node representation. One Node type can customize its own node
  // representation.
  virtual std::vector<Dot::Attr> dot_attrs() const {
    return std::vector<Dot::Attr>({Dot::Attr("style", "filled")});
  }

  // Get an additional attribute and convert it to T data type. NOTE this will
  // silently create a new attribute if not exists.
  AnyAttr &attr(const std::string &name) const { return attrs_[name]; }

  int id() const { return id_; }

  // The Protobuf description is set/get with a void* to decouple Node interface
  // from a specific kind of Protobuf message.
  void SetPbDesc(void *pb) { attr("pb_desc").Pointer() = pb; }
  void *pb_desc() const { return attr("pb_desc").Pointer(); }

  void SetPbMsg(const std::string &s) { attr("pb_msg").String() = s; }
  const std::string &pb_msg() const { return attr("pb_msg").String(); }

  void SetDeleted() { deleted_ = true; }
  bool deleted() const { return deleted_; }

  void SetName(const std::string &name) { name_ = name; }
  const std::string &name() const { return name_; }

  void SetType(Type type) { type_ = type; }
  Type type() const { return type_; }

  // Input links.
  std::vector<Node *> inlinks;
  // Output links.
  std::vector<Node *> outlinks;

  // Type checks.
  bool IsFunction() const { return type_ == Node::Type::kFunction; }
  bool IsValue() const { return type_ == Node::Type::kValue; }
  bool IsFunctionBlock() const { return type_ == Node::Type::kFunctionBlock; }

  virtual ~Node() {}

  friend class NodeMap;

  PADDLE_DISALLOW_COPY_AND_ASSIGN(Node);

 protected:
  // The id number not the name is a node's unique identifier in the computation
  // graph.
  int id_{-1};
  std::string name_;
  Type type_{Type::kNone};
  // Mark this node is deleted by some pass.
  bool deleted_{false};
  mutable std::unordered_map<std::string, AnyAttr> attrs_;
};

class Function;
/*
 * Value represents a value node, it has some attributes including dims, data
 * type and so on.
 */
class Value : public Node {
 public:
  enum class DataType { kInt32, kInt64, kFloat32, kFloat64 };
  using Dims = std::vector<int>;

  void SetDataType(DataType data_type) { data_type_ = data_type; }
  DataType data_type() const { return data_type_; }

  void SetDims(const Dims &dims) { dims_ = dims; }
  const Dims &dims() const { return dims_; }

  Device device() const { return device_; }
  void SetDevice(Device device) { device_ = device; }

  std::vector<Dot::Attr> dot_attrs() const override;

  PADDLE_DISALLOW_COPY_AND_ASSIGN(Value);

 protected:
  Value() { SetType(Node::Type::kValue); }
  friend class NodeMap;

 private:
  DataType data_type_;
  Dims dims_;
  Device device_;
};

/*
 * Function represents any kind of executable concepts that takes several Values
 * as input, and outputs several Values.
 */
class Function : public Node {
 public:
  std::vector<Dot::Attr> dot_attrs() const override;

  // Get the operator's type from Desc.
  const std::string &func_type() const { return func_type_; }
  // Set the operator's type.
  void SetFuncType(const std::string &func_type) { func_type_ = func_type; }

  PADDLE_DISALLOW_COPY_AND_ASSIGN(Function);

 protected:
  std::string func_type_;
  Function() { SetType(Node::Type::kFunction); }
  friend class NodeMap;
};

/*
 * FunctionBlock is a Node that contains a sub-graph multiple Node.
 */
struct FunctionBlock : public Node {
  std::string repr() const override { return "block-" + std::to_string(id()); }
  std::vector<Node *> subgraph;

 protected:
  FunctionBlock() { SetType(Node::Type::kFunctionBlock); }
  friend class NodeMap;
};

class NodeMap {
 public:
  // Create a new node with type.
  Node *Create(Node::Type type);

  // Get a node by its id.
  Node *GetMutable(size_t id);

  const Node &Get(size_t id) const;

  void Delete(size_t id);

  const std::vector<std::unique_ptr<Node>> &nodes() const { return nodes_; }

  size_t size() const { return nodes_.size(); }

 private:
  std::vector<std::unique_ptr<Node>> nodes_;
  std::unordered_map<std::string, Node *> map_;
};

}  // namespace analysis
}  // namespace inference
}  // namespace paddle
