// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/dialect/pd_attribute.h"

namespace paddle {
namespace dialect {
phi::IntArray IntArrayAttribute::data() const { return storage()->GetAsKey(); }

paddle::experimental::Scalar ScalarAttribute::data() const {
  return storage()->GetAsKey();
}

phi::DataType DataTypeAttribute::data() const { return storage()->GetAsKey(); }

phi::Place PlaceAttribute::data() const { return storage()->GetAsKey(); }

phi::DataLayout DataLayoutAttribute::data() const {
  return storage()->GetAsKey();
}

}  // namespace dialect
}  // namespace paddle
