// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// IWYU pragma: private, include "sus/iter/iterator.h"
// IWYU pragma: friend "sus/.*"
#pragma once

#include "fmt/format.h"
#include "sus/iter/size_hint.h"
#include "sus/num/unsigned_integer_impl.h"  // For formatting usize.
#include "sus/option/option.h"              // For formatting option.
#include "sus/string/__private/format_to_stream.h"

// fmt support.
template <class Char>
template <class FormatContext>
FormatContext::iterator fmt::formatter<::sus::iter::SizeHint, Char>::format(
    const ::sus::iter::SizeHint& t, FormatContext& ctx) const {
  return fmt::format_to(ctx.out(), "SizeHint({}, {})", t.lower, t.upper);
}
