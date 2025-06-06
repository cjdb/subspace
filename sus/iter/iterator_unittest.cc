// Copyright 2022 Google LLC
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

#include "sus/iter/iterator.h"

#include <ranges>

#include "googletest/include/gtest/gtest.h"
#include "sus/assertions/unreachable.h"
#include "sus/cmp/eq.h"
#include "sus/collections/array.h"
#include "sus/collections/vec.h"
#include "sus/construct/into.h"
#include "sus/iter/__private/into_iterator_archetype.h"
#include "sus/iter/__private/iterator_archetype.h"
#include "sus/iter/adaptors/filter.h"
#include "sus/iter/empty.h"
#include "sus/iter/into_iterator.h"
#include "sus/iter/zip.h"
#include "sus/macros/__private/compiler_bugs.h"
#include "sus/mem/never_value.h"
#include "sus/mem/replace.h"
#include "sus/num/overflow_integer.h"
#include "sus/prelude.h"
#include "sus/test/no_copy_move.h"

using sus::collections::Array;
using sus::iter::IteratorBase;
using sus::option::Option;
using sus::result::Result;
using sus::test::NoCopyMove;

namespace sus::test::iter {

template <class T>
struct CollectSum {
  T sum;
};

struct CollectRefs {
  sus::Vec<const NoCopyMove*> vec;
};

}  // namespace sus::test::iter

template <class T>
struct sus::iter::FromIteratorImpl<sus::test::iter::CollectSum<T>> {
  static constexpr sus::test::iter::CollectSum<T> from_iter(
      sus::iter::IntoIterator<T> auto&& into_iter) noexcept {
    T sum = T();
    for (T t : sus::move(into_iter).into_iter()) sum += t;
    return sus::test::iter::CollectSum<T>(sum);
  }
};

template <>
struct sus::iter::FromIteratorImpl<sus::test::iter::CollectRefs> {
  static sus::test::iter::CollectRefs from_iter(
      sus::iter::IntoIterator<const NoCopyMove&> auto into_iter) noexcept {
    auto v = sus::Vec<const NoCopyMove*>();
    for (const NoCopyMove& t : sus::move(into_iter).into_iter()) v.push(&t);
    return sus::test::iter::CollectRefs(sus::move(v));
  }
};

namespace {
using namespace sus::test::iter;

static_assert(::sus::cmp::Eq<sus::iter::SizeHint>);

static_assert(
    sus::iter::IteratorAny<sus::iter::__private::IteratorArchetype<int>>);
static_assert(
    sus::iter::Iterator<sus::iter::__private::IteratorArchetype<int>, int>);
static_assert(sus::iter::IntoIterator<
              sus::iter::__private::IntoIteratorArchetype<int>, int>);

// clang-format off
static_assert(
  ::sus::iter::Iterator<sus::iter::Empty<int>, int>);
static_assert(
  sus::iter::Iterator<sus::iter::Filter<sus::iter::Empty<int>, decltype([](int){return true;})>, int>);
static_assert(
  sus::iter::Iterator<sus::iter::Map<bool, sus::iter::Empty<int>, decltype([](int){return true;})>, bool>);
static_assert(
  sus::iter::Iterator<sus::iter::MapWhile<bool, sus::iter::Empty<int>, decltype([](int){return sus::Option<bool>(true);})>, bool>);
static_assert(
  sus::iter::Iterator<sus::iter::Once<int>, int>);
static_assert(
  sus::iter::Iterator<sus::collections::ArrayIntoIter<int, 1>, int>);
static_assert(
  sus::iter::Iterator<sus::collections::VecIntoIter<int>, int>);
static_assert(
  sus::iter::Iterator<sus::collections::SliceIter<const int&>, const int&>);
static_assert(
  sus::iter::Iterator<sus::collections::SliceIterMut<int&>, int&>);

static_assert(
  sus::iter::DoubleEndedIterator<sus::collections::ArrayIntoIter<int, 1>, int>);
static_assert(
  sus::iter::DoubleEndedIterator<sus::collections::SliceIter<const int&>, const int&>);
static_assert(
  sus::iter::DoubleEndedIterator<sus::collections::SliceIterMut<int&>, int&>);
static_assert(
  sus::iter::DoubleEndedIterator<sus::collections::VecIntoIter<int>, int>);

static_assert(
  sus::iter::TrustedLen<sus::collections::ArrayIntoIter<int, 1>>);
static_assert(
  sus::iter::TrustedLen<sus::collections::SliceIter<const int&>>);
static_assert(
  sus::iter::TrustedLen<sus::collections::SliceIterMut<int&>>);
static_assert(
  sus::iter::TrustedLen<sus::collections::VecIntoIter<int>>);

static_assert(
  sus::iter::IntoIterator<sus::collections::Array<int, 3u>, int>);
static_assert(
  sus::iter::IntoIterator<sus::collections::Slice<int>, const int&>);
static_assert(
  sus::iter::IntoIterator<sus::collections::SliceMut<int>, int&>);
static_assert(
  sus::iter::IntoIterator<sus::collections::Vec<int>, int>);

static_assert(
  sus::iter::IntoIterator<sus::iter::Once<int>, int>);
// clang-format on

template <class Item, size_t N>
class ArrayIterator final : public IteratorBase<ArrayIterator<Item, N>, Item> {
 public:
  static ArrayIterator with_array(Item (&items)[N]) noexcept {
    return ArrayIterator(items);
  }

  // sus::iter::Iterator trait.
  Option<Item> next() noexcept {
    if (front_ >= back_) return sus::none();
    return items_
        .get_unchecked_mut(::sus::marker::unsafe_fn,
                           sus::mem::replace(front_, front_ + 1u))
        .take();
  }

  // sus::iter::DoubleEndedIterator trait.
  Option<Item> next_back() noexcept {
    if (front_ >= back_) return sus::none();
    back_ -= 1u;
    return items_.get_unchecked_mut(::sus::marker::unsafe_fn, back_).take();
  }

  sus::iter::SizeHint size_hint() const noexcept {
    return sus::iter::SizeHint(exact_size_hint(), sus::some(exact_size_hint()));
  }

  // sus::iter::ExactSizeIterator trait.
  usize exact_size_hint() const noexcept {
    // SAFETY: This requires `back_ - front_` is in range which is guaranteed by
    // `back_ >= front_`. This is true since `front_` starts from 0 and `back_`
    // from N with `0 <= N`. Then `front_` is incremented and `back_` is
    // decremented, but when `front_ == back_` neither is moved thereafter.
    return back_.unchecked_sub(::sus::marker::unsafe_fn, front_);
  }

  /// sus::iter::TrustedLen trait.
  constexpr sus::iter::__private::TrustedLenMarker trusted_len()
      const noexcept {
    return {};
  }

 protected:
  ArrayIterator(Item (&items)[N])
      : items_(Array<Option<Item>, N>::with_initializer(
            [&items, i = 0_usize]() mutable -> Option<Item> {
              return Option<Item>(
                  sus::move(*(items + ::sus::mem::replace(i, i + 1u))));
            })) {}

 private:
  usize front_;
  usize back_ = N;
  Array<Option<Item>, N> items_;

  sus_class_trivially_relocatable_if_types(unsafe_fn, decltype(front_),
                                           decltype(back_), decltype(items_));
};

static_assert(sus::iter::Iterator<ArrayIterator<int, 1>, int>);

template <class Item>
class EmptyIterator final : public IteratorBase<EmptyIterator<Item>, Item> {
 public:
  EmptyIterator() {}

  // sus::iter::Iterator trait.
  Option<Item> next() noexcept { return Option<Item>(); }
};

TEST(Iterator, ForLoop) {
  int nums[5] = {1, 2, 3, 4, 5};

  auto it_lvalue = ArrayIterator<int, 5>::with_array(nums);
  int count = 0;
  for (auto i : it_lvalue) {
    static_assert(std::is_same_v<decltype(i), int>, "");
    EXPECT_EQ(i, ++count);
  }
  EXPECT_EQ(count, 5);

  count = 0;
  for (auto i : ArrayIterator<int, 5>::with_array(nums)) {
    static_assert(std::is_same_v<decltype(i), int>, "");
    EXPECT_EQ(i, ++count);
  }
  EXPECT_EQ(count, 5);
}

TEST(Iterator, All) {
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_TRUE(it.all([](int i) { return i <= 5; }));
  }
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_FALSE(it.all([](int i) { return i <= 4; }));
  }
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_FALSE(it.all([](int i) { return i <= 0; }));
  }

  // Shortcuts at the first failure.
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_FALSE(it.all([](int i) { return i <= 3; }));
    Option<int> n = it.next();
    ASSERT_TRUE(n.is_some());
    // The `all()` function stopped when it consumed 4, so we can still consume
    // 5.
    EXPECT_EQ(n.as_ref().unwrap(), 5);
  }

  {
    auto it = EmptyIterator<int>();
    EXPECT_TRUE(it.all([](int) { return false; }));
  }

  static_assert(sus::Array<i32, 3>(2, 3, 4).into_iter().all(
                    [](auto i) { return i <= 4; }) == true);
}

TEST(Iterator, Any) {
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_TRUE(it.any([](int i) { return i == 5; }));
  }
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_FALSE(it.any([](int i) { return i == 6; }));
  }
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_TRUE(it.any([](int i) { return i == 1; }));
  }

  // Shortcuts at the first success.
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_TRUE(it.any([](int i) { return i == 3; }));
    Option<int> n = it.next();
    ASSERT_TRUE(n.is_some());
    // The `any()` function stopped when it consumed 3, so we can still consume
    // 4.
    EXPECT_EQ(n.as_ref().unwrap(), 4);
  }

  {
    auto it = EmptyIterator<int>();
    EXPECT_FALSE(it.any([](int) { return false; }));
  }

  static_assert(sus::Array<i32, 3>(2, 3, 4).into_iter().any(
                    [](auto i) { return i > 4; }) == false);
}

TEST(Iterator, Count) {
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_EQ(sus::move(it).count(), 5_usize);
  }
  {
    int nums[2] = {4, 5};
    auto it = ArrayIterator<int, 2>::with_array(nums);
    EXPECT_EQ(sus::move(it).count(), 2_usize);
  }
  {
    int nums[1] = {2};
    auto it = ArrayIterator<int, 1>::with_array(nums);
    EXPECT_EQ(sus::move(it).count(), 1_usize);
  }

  // Consumes the whole iterator.
  {
    int nums[5] = {1, 2, 3, 4, 5};
    auto it = ArrayIterator<int, 5>::with_array(nums);
    EXPECT_EQ(sus::move(it).count(), 5_usize);
    Option<int> n = it.next();
    ASSERT_FALSE(n.is_some());
  }

  {
    auto it = EmptyIterator<int>();
    EXPECT_EQ(sus::move(it).count(), 0_usize);
  }
}

TEST(Iterator, Filter) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  auto fit = ArrayIterator<i32, 5>::with_array(nums).filter(
      [](const i32& i) { return i >= 3; });
  EXPECT_EQ(sus::move(fit).count(), 3_usize);

  auto fit2 = ArrayIterator<i32, 5>::with_array(nums)
                  .filter([](const i32& i) { return i >= 3; })
                  .filter([](const i32& i) { return i <= 4; });
  i32 expect = 3;
  for (i32 i : fit2) {
    EXPECT_EQ(expect, i);
    expect += 1;
  }
  EXPECT_EQ(expect, 5);

  static_assert(sus::Vec<i32>(2, 3, 4, 5, 6)
                    .into_iter()
                    .filter([](i32 i) { return i >= 3 && i <= 5; })
                    .sum() == 3 + 4 + 5);
}

TEST(Iterator, FilterMap) {
  auto it = sus::Vec<i32>(1, 2, 3, 4, 5).into_iter();
  auto fmit = sus::move(it).filter_map([](i32&& i) -> sus::Option<u32> {
    if (i >= 3) {
      return u32::try_from(i).ok();
    } else {
      return sus::none();
    }
  });
  static_assert(std::same_as<decltype(fmit.next()), sus::Option<u32>>);

  EXPECT_EQ(fmit.size_hint(), sus::iter::SizeHint(0u, sus::some(5_usize)));

  static_assert(sus::Vec<i32>(2, 3, 4, 5, 6)
                    .into_iter()
                    .filter_map([](i32 i) -> Option<u32> {
                      if (i >= 3 && i <= 5)
                        return sus::some(sus::cast<u32>(i));
                      else
                        return sus::none();
                    })
                    .sum() == 3u + 4u + 5u);
}

TEST(Iterator, FilterDoubleEnded) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  auto it = ArrayIterator<i32, 5>::with_array(nums).filter(
      [](const i32& i) { return i == 2 || i == 4; });
  EXPECT_EQ(it.next_back(), sus::some(4_i32));
  EXPECT_EQ(it.next_back(), sus::some(2_i32));
  EXPECT_EQ(it.next_back(), sus::None);
}

struct Filtering {
  Filtering(i32 i) : i(i) {}
  Filtering(Filtering&& f) : i(f.i) {}
  Filtering& operator=(Filtering&& f) {
    i = f.i;
    return *this;
  }
  ~Filtering() {}
  i32 i;
};
static_assert(!sus::mem::TriviallyRelocatable<Filtering>);

TEST(Iterator, FilterNonTriviallyRelocatable) {
  Filtering nums[5] = {Filtering(1), Filtering(2), Filtering(3), Filtering(4),
                       Filtering(5)};

  auto non_relocatable_it = ArrayIterator<Filtering, 5>::with_array(nums);
  static_assert(!sus::mem::TriviallyRelocatable<decltype(non_relocatable_it)>);

  // Previously we needed iterators to be trivially relocatable to nest them in
  // an adaptor iterator like filter(). So this test would call box() to move
  // the iterator to the heap. That is no longer case, and this test now shows
  // it is not needed.
  auto fit = sus::move(non_relocatable_it).filter([](const Filtering& f) {
    return f.i >= 3;
  });
  EXPECT_EQ(sus::move(fit).count(), 3_usize);
}

TEST(Iterator, Map) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  auto it = ArrayIterator<i32, 5>::with_array(nums).map(
      [](i32&& i) { return u32::try_from(i).unwrap(); });
  auto v = sus::move(it).collect_vec();
  static_assert(std::same_as<decltype(v), sus::Vec<u32>>);
  {
    u32 expect = 1u;
    for (u32 i : v) {
      EXPECT_EQ(expect, i);
      expect += 1u;
    }
  }

  struct MapOut {
    u32 val;
  };

  auto it2 = ArrayIterator<i32, 5>::with_array(nums)
                 .map([](i32&& i) { return u32::try_from(i).unwrap(); })
                 .map([](u32&& i) { return MapOut(i); });
  auto v2 = sus::move(it2).collect_vec();
  static_assert(std::same_as<decltype(v2), sus::Vec<MapOut>>);
  {
    u32 expect = 1u;
    for (const MapOut& i : v2) {
      EXPECT_EQ(expect, i.val);
      expect += 1u;
    }
  }

  static_assert(sus::Array<i32, 3>(2, 3, 4)
                    .into_iter()
                    .map([](i32 i) { return u32::try_from(i).unwrap() + 1u; })
                    .sum() == 3u + 4u + 5u);
}

TEST(Iterator, MapDoubleEnded) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  auto it = ArrayIterator<i32, 5>::with_array(nums).map(
      [](i32&& i) { return u32::try_from(i).unwrap(); });
  EXPECT_EQ(it.next_back(), sus::some(5_u32));
  EXPECT_EQ(it.next_back(), sus::some(4_u32));
  EXPECT_EQ(it.next_back(), sus::some(3_u32));
  EXPECT_EQ(it.next_back(), sus::some(2_u32));
  EXPECT_EQ(it.next_back(), sus::some(1_u32));
  EXPECT_EQ(it.next_back(), sus::None);
}

TEST(Iterator, MapWhile) {
  {
    auto nums = sus::Array<i32, 5>(1, 2, 3, 4, 5);
    auto it = sus::move(nums).into_iter().map_while([](i32&& i) -> Option<u32> {
      if (i != 4)
        return u32::try_from(i).ok();
      else
        return sus::none();
    });
    static_assert(std::same_as<decltype(it.next()), sus::Option<u32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(5u)));
    EXPECT_EQ(it.next(), sus::some(1u));
    EXPECT_EQ(it.next(), sus::some(2u));
    EXPECT_EQ(it.next(), sus::some(3u));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next(), sus::some(5u));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
  }

  static_assert(sus::Array<i32, 5>(2, 3, 4, -1, 5)
                    .into_iter()
                    .map_while([](i32 i) { return u32::try_from(i).ok(); })
                    .sum() == 2u + 3u + 4u);
}

TEST(Iterator, Collect) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  auto collected =
      ArrayIterator<i32, 5>::with_array(nums).collect<CollectSum<i32>>();
  EXPECT_EQ(collected.sum, 1 + 2 + 3 + 4 + 5);

  auto from = sus::iter::from_iter<CollectSum<i32>>(
      ArrayIterator<i32, 5>::with_array(nums));
  EXPECT_EQ(from.sum, 1 + 2 + 3 + 4 + 5);

  static_assert(sus::Array<i32, 5>(1, 2, 3, 4, 5)
                    .into_iter()
                    .collect<CollectSum<i32>>()
                    .sum == 1 + 2 + 3 + 4 + 5);
}

TEST(Iterator, CollectVec) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  auto collected = ArrayIterator<i32, 5>::with_array(nums).collect_vec();
  static_assert(std::same_as<decltype(collected), Vec<i32>>);
  EXPECT_EQ(collected.len(), 5u);
  EXPECT_EQ(collected[0u], 1);
  EXPECT_EQ(collected[2u], 3);
  EXPECT_EQ(collected[4u], 5);

  static_assert(sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter().collect_vec() ==
                sus::Vec<i32>(1, 2, 3, 4, 5));
}

TEST(Iterator, TryCollect) {
  // Option.
  {
    auto collected = sus::Array<Option<i32>, 3>(::sus::some(1), ::sus::some(2),
                                                ::sus::some(3))
                         .into_iter()
                         .try_collect<Vec<i32>>();
    static_assert(std::same_as<decltype(collected), Option<Vec<i32>>>);
    EXPECT_EQ(collected.is_some(), true);
    EXPECT_EQ(collected.as_value(), sus::Vec<i32>(1, 2, 3));

    auto it = sus::Array<Option<i32>, 3>(::sus::some(1), ::sus::none(),
                                         ::sus::some(3))
                  .into_iter();
    auto up_to_none = it.try_collect<Vec<i32>>();
    EXPECT_EQ(up_to_none, sus::none());
    auto after_none = it.try_collect<Vec<i32>>();
    EXPECT_EQ(after_none.as_value(), sus::Vec<i32>(3));

    NoCopyMove n[3];
    auto refs = sus::Array<Option<const NoCopyMove&>, 3>(
                    ::sus::some(n[0]), ::sus::some(n[1]), ::sus::some(n[2]))
                    .into_iter()
                    .try_collect<CollectRefs>();
    EXPECT_EQ(refs.is_some(), true);
    EXPECT_EQ(refs.as_value().vec[0u], &n[0]);
    EXPECT_EQ(refs.as_value().vec[1u], &n[1]);
    EXPECT_EQ(refs.as_value().vec[2u], &n[2]);
  }
  // Result.
  enum Error { ERROR };
  {
    auto collected = sus::Array<Result<i32, Error>, 3>(
                         ::sus::ok(1), ::sus::ok(2), ::sus::ok(3))
                         .into_iter()
                         .try_collect<Vec<i32>>();
    static_assert(std::same_as<decltype(collected), Result<Vec<i32>, Error>>);
    EXPECT_EQ(collected.as_value(), sus::Vec<i32>(1, 2, 3));

    auto it = sus::Array<Result<i32, Error>, 3>(::sus::ok(1), ::sus::err(ERROR),
                                                ::sus::ok(3))
                  .into_iter();
    auto up_to_err = it.try_collect<Vec<i32>>();
    EXPECT_EQ(up_to_err, sus::err(ERROR));
    auto after_err = it.try_collect<Vec<i32>>();
    EXPECT_EQ(after_err.as_value(), sus::Vec<i32>(3));
  }

  auto from = sus::iter::try_from_iter<Vec<i32>>(sus::Array<Option<i32>, 3>(
      ::sus::some(1), ::sus::some(2), ::sus::some(3)));
  EXPECT_EQ(from.as_value(), sus::Vec<i32>(1, 2, 3));

  static_assert(
      sus::Array<Option<i32>, 3>(sus::some(1), sus::some(2), sus::some(3))
          .into_iter()
          .try_collect<Vec<i32>>()
          .unwrap()
          .into_iter()
          .sum() == 1 + 2 + 3);
}

TEST(Iterator, TryCollect_Example) {
  using sus::err;
  using sus::none;
  using sus::ok;
  using sus::Option;
  using sus::some;
  using sus::Vec;
  using sus::result::Result;
  {
    auto u = Vec<Option<i32>>(some(1), some(2), some(3));
    auto v = sus::move(u).into_iter().try_collect<Vec<i32>>();
    sus_check(v == some(Vec<i32>(1, 2, 3)));
  }
  {
    auto u = Vec<Option<i32>>(some(1), some(2), none(), some(3));
    auto v = sus::move(u).into_iter().try_collect<Vec<i32>>();
    sus_check(v == none());
  }
  {
    enum Error { ERROR };
    auto u = Vec<Result<i32, Error>>(ok(1), ok(2), ok(3));
    auto v = sus::move(u).into_iter().try_collect<Vec<i32>>();
    sus_check(v == ok(Vec<i32>(1, 2, 3)));
    auto w = Vec<Result<i32, Error>>(ok(1), ok(2), err(ERROR), ok(3));
    auto x = sus::move(w).into_iter().try_collect<Vec<i32>>();
    sus_check(x == err(ERROR));
  }
}

TEST(Iterator, Rev) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  auto it = ArrayIterator<i32, 5>::with_array(nums).rev();
  static_assert(sus::iter::Iterator<decltype(it), i32>);
  static_assert(sus::iter::DoubleEndedIterator<decltype(it), i32>);
  static_assert(sus::iter::ExactSizeIterator<decltype(it), i32>);
  EXPECT_EQ(it.next(), sus::some(5));
  EXPECT_EQ(it.next(), sus::some(4));
  EXPECT_EQ(it.next(), sus::some(3));
  EXPECT_EQ(it.next(), sus::some(2));
  EXPECT_EQ(it.next(), sus::some(1));
  EXPECT_EQ(it.next(), sus::None);

  static_assert(
      sus::Vec<i32>(1, 2, 3).into_iter().rev().collect<sus::Vec<i32>>() ==
      sus::Vec<i32>(3, 2, 1));
}

TEST(Iterator, Enumerate) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  {
    auto it = ArrayIterator<i32, 5>::with_array(nums).rev().enumerate();
    using E = sus::Tuple<usize, i32>;
    static_assert(sus::iter::Iterator<decltype(it), E>);
    static_assert(sus::iter::DoubleEndedIterator<decltype(it), E>);
    static_assert(sus::iter::ExactSizeIterator<decltype(it), E>);
    EXPECT_EQ(it.next().unwrap(), sus::Tuple(0u, 5));
    EXPECT_EQ(it.next().unwrap(), sus::Tuple(1u, 4));
    EXPECT_EQ(it.next().unwrap(), sus::Tuple(2u, 3));
    EXPECT_EQ(it.next().unwrap(), sus::Tuple(3u, 2));
    EXPECT_EQ(it.next().unwrap(), sus::Tuple(4u, 1));
    EXPECT_EQ(it.next(), sus::None);
  }
  {
    auto it = ArrayIterator<i32, 5>::with_array(nums).enumerate();
    using E = sus::Tuple<usize, i32>;
    static_assert(sus::iter::Iterator<decltype(it), E>);
    static_assert(sus::iter::DoubleEndedIterator<decltype(it), E>);
    static_assert(sus::iter::ExactSizeIterator<decltype(it), E>);
    EXPECT_EQ(it.next_back().unwrap(), sus::Tuple(4u, 5));
    EXPECT_EQ(it.next_back().unwrap(), sus::Tuple(3u, 4));
    EXPECT_EQ(it.next_back().unwrap(), sus::Tuple(2u, 3));
    EXPECT_EQ(it.next_back().unwrap(), sus::Tuple(1u, 2));
    EXPECT_EQ(it.next_back().unwrap(), sus::Tuple(0u, 1));
    EXPECT_EQ(it.next_back(), sus::None);
  }

  auto vec = Vec<i32>(0, 2, 4, 6, 8);
  // Front to back.
  for (auto [i, value] : vec.iter().enumerate()) {
    EXPECT_EQ(i * 2u, usize::try_from(value).unwrap());
  }
  // Back to front.
  for (auto [i, value] : vec.iter().enumerate().rev()) {
    EXPECT_EQ(i * 2u, usize::try_from(value).unwrap());
  }
  // Back to front without reversing the indices.
  for (auto [i, value] : vec.iter().rev().enumerate()) {
    EXPECT_EQ((4u - i) * 2u, usize::try_from(value).unwrap());
  }

  static_assert(
      sus::Vec<i32>(2, 3, 4)
          .into_iter()
          .enumerate()
          // Create an iterator over the enumerate indices, to sum it.
          .unzip<sus::Vec<usize>, sus::Vec<i32>>()
          // Grabs the iterator over indices, discards the vector values.
          .into_inner<0>()
          .into_iter()
          // Sum the indices.
          .sum() == 0u + 1u + 2u);
}

TEST(Iterator, ByRef) {
  i32 nums[5] = {1, 2, 3, 4, 5};

  {
    auto it = ArrayIterator<i32, 5>::with_array(nums);
    {
      auto refit = it.by_ref();
      EXPECT_EQ(refit.next().unwrap(), 1);
      EXPECT_EQ(refit.next().unwrap(), 2);
    }
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.next().unwrap(), 5);
    EXPECT_EQ(it.next(), sus::None);
  }
  {
    auto it = ArrayIterator<i32, 5>::with_array(nums);
    {
      // The `rev()` applies to the ByRef iterator but not the original one.
      auto refit = it.by_ref().rev();
      EXPECT_EQ(refit.next().unwrap(), 5);
      EXPECT_EQ(refit.next().unwrap(), 4);
    }
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.next(), sus::None);
  }

  static_assert([]() {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter();
    return it.by_ref().any([](auto i) { return i >= 4; });
  }() == true);
}

TEST(Iterator, Chain) {
  i32 nums1[] = {1, 2, 3};
  i32 nums2[] = {4, 5};

  {
    auto it = ArrayIterator<i32, 3>::with_array(nums1);
    auto it2 = ArrayIterator<i32, 2>::with_array(nums2);

    // Make an iterator [1, 2, 3, 5, 4].
    auto c = std::move(it).chain(std::move(it2).rev());
    EXPECT_EQ(c.size_hint().lower, 5u);
    EXPECT_EQ(*c.size_hint().upper, 5u);

    EXPECT_EQ(c.next().unwrap(), 1);
    EXPECT_EQ(c.next_back().unwrap(), 4);
    EXPECT_EQ(c.next().unwrap(), 2);

    EXPECT_EQ(c.size_hint().lower, 2u);
    EXPECT_EQ(*c.size_hint().upper, 2u);

    EXPECT_EQ(c.next_back().unwrap(), 5);
    EXPECT_EQ(c.next().unwrap(), 3);

    EXPECT_EQ(c.size_hint().lower, 0u);
    EXPECT_EQ(*c.size_hint().upper, 0u);

    EXPECT_EQ(c.next(), sus::None);

    EXPECT_EQ(c.size_hint().lower, 0u);
    EXPECT_EQ(*c.size_hint().upper, 0u);
  }

  static_assert(sus::Vec<i32>(2, 3, 4).len() == 3u);

  static_assert(
      sus::Vec<i32>(2, 3, 4).into_iter().chain(sus::Vec<i32>(5, 6)).count() ==
      5u);
}

TEST(Iterator, ChainFromIterator) {
  auto v1 = sus::Vec<i32>(1, 2);
  auto v2 = sus::Vec<i32>(3);
  sus::collections::VecIntoIter<i32> iti = sus::move(v1).into_iter();
  auto it = sus::move(iti).chain(sus::move(v2));
  EXPECT_EQ(it.next().unwrap(), 1);
  EXPECT_EQ(it.next().unwrap(), 2);
  EXPECT_EQ(it.next().unwrap(), 3);
  EXPECT_EQ(it.next(), sus::None);
}

TEST(Iterator, Cloned) {
  static usize clone_called;
  struct Cloning {
    Cloning() = default;
    Cloning(Cloning&&) = default;
    Cloning& operator=(Cloning&&) = default;
    Cloning clone() const noexcept { return clone_called += 1u, Cloning(); }
  };
  static_assert(sus::mem::Clone<Cloning>);

  auto cloning = sus::Vec<Cloning>(Cloning(), Cloning());

  // Clone from references.
  {
    auto it = cloning.iter().cloned();
    static_assert(std::same_as<Cloning, decltype(it.next().unwrap())>);
    EXPECT_EQ(it.size_hint().lower, 2u);
    EXPECT_EQ(it.size_hint().upper.unwrap(), 2u);

    EXPECT_EQ(clone_called, 0u);
    EXPECT_EQ(it.next().is_some(), true);
    EXPECT_EQ(clone_called, 1u);

    EXPECT_EQ(it.next().is_some(), true);
    EXPECT_EQ(clone_called, 2u);

    EXPECT_EQ(it.next().is_some(), false);
    EXPECT_EQ(clone_called, 2u);
  }
  clone_called = 0u;

  // Clone from values.
  {
    auto it = sus::move(cloning).into_iter().cloned();
    static_assert(std::same_as<Cloning, decltype(it.next().unwrap())>);
    EXPECT_EQ(it.size_hint().lower, 2u);
    EXPECT_EQ(it.size_hint().upper.unwrap(), 2u);

    EXPECT_EQ(clone_called, 0u);
    EXPECT_EQ(it.next().is_some(), true);
    EXPECT_EQ(clone_called, 1u);

    EXPECT_EQ(it.next().is_some(), true);
    EXPECT_EQ(clone_called, 2u);

    EXPECT_EQ(it.next().is_some(), false);
    EXPECT_EQ(clone_called, 2u);
  }

  static_assert(sus::Vec<usize>(1u, 2u).into_iter().cloned().sum() == 1u + 2u);
}

TEST(Iterator, Copied) {
  auto copying = sus::Vec<usize>(1u, 2u);

  // Copy from references.
  {
    auto it = copying.iter().cloned();
    static_assert(std::same_as<usize, decltype(it.next().unwrap())>);
    EXPECT_EQ(it.size_hint().lower, 2u);
    EXPECT_EQ(it.size_hint().upper.unwrap(), 2u);

    EXPECT_EQ(it.next().unwrap(), 1u);
    EXPECT_EQ(it.next().unwrap(), 2u);
    EXPECT_EQ(it.next(), sus::None);
  }

  // Copy from values.
  {
    auto it = sus::move(copying).into_iter().cloned();
    static_assert(std::same_as<usize, decltype(it.next().unwrap())>);
    EXPECT_EQ(it.size_hint().lower, 2u);
    EXPECT_EQ(it.size_hint().upper.unwrap(), 2u);

    EXPECT_EQ(it.next().unwrap(), 1u);
    EXPECT_EQ(it.next().unwrap(), 2u);
    EXPECT_EQ(it.next(), sus::None);
  }

  static_assert(sus::Vec<usize>(1u, 2u).into_iter().copied().sum() == 1u + 2u);
}

TEST(Iterator, StrongCmp) {
  {
    auto smol = sus::Vec<i32>(1, 2);
    auto bigg = sus::Vec<i32>(1, 2, 1);
    EXPECT_EQ(std::strong_ordering::less, smol.iter().strong_cmp(bigg.iter()));
  }
  {
    auto bigg = sus::Vec<i32>(1, 2, 1);
    auto smol = sus::Vec<i32>(1, 2);
    EXPECT_EQ(std::strong_ordering::greater,
              bigg.iter().strong_cmp(smol.iter()));
  }
  {
    auto smol = sus::Vec<i32>(1, 2);
    auto bigg = sus::Vec<i32>(1, 1, 1);
    EXPECT_EQ(std::strong_ordering::greater,
              smol.iter().strong_cmp(bigg.iter()));
  }
  {
    auto smol = sus::Vec<i32>(1, 2);
    auto bigg = sus::Vec<i32>(1, 2);
    EXPECT_EQ(std::strong_ordering::equal, smol.iter().strong_cmp(bigg.iter()));
  }

  // iter_mut.
  {
    auto smol = sus::Vec<i32>(1, 3);
    auto bigg = sus::Vec<i32>(1, 2);
    EXPECT_EQ(std::strong_ordering::greater,
              smol.iter_mut().strong_cmp(bigg.iter_mut()));
  }

  // into_iter.
  {
    auto smol = sus::Vec<i32>(1, 3);
    auto bigg = sus::Vec<i32>(1, 2);
    EXPECT_EQ(
        std::strong_ordering::greater,
        sus::move(smol).into_iter().strong_cmp(sus::move(bigg).into_iter()));
  }

  // Comparable but different types.
  {
    auto one = sus::Vec<i32>(1, 2);
    auto two = sus::Vec<i64>(1, 3);
    EXPECT_EQ(std::strong_ordering::less, one.iter().strong_cmp(two.iter()));
  }

  static_assert(sus::Array<i32, 2>(1, 2).into_iter().strong_cmp(
                    sus::Array<i32, 2>(1, 3)) == std::strong_ordering::less);
  static_assert(sus::Array<i32, 3>(1, 2, 0).into_iter().strong_cmp(
                    sus::Array<i32, 2>(1, 2)) == std::strong_ordering::greater);
}

TEST(Iterator, StrongCmpBy) {
  {
    auto bigg = sus::Vec<i32>(1, 2, 1);
    auto smol = sus::Vec<i32>(1, 2);
    EXPECT_EQ(
        std::strong_ordering::less,
        smol.iter().strong_cmp_by(
            bigg.iter(), [](const i32& a, const i32& b) { return b <=> a; }));
  }
  {
    auto smol = sus::Vec<i32>(1, 2);
    auto bigg = sus::Vec<i32>(1, 2, 1);
    EXPECT_EQ(
        std::strong_ordering::greater,
        bigg.iter().strong_cmp_by(
            smol.iter(), [](const i32& a, const i32& b) { return b <=> a; }));
  }
  {
    auto smol = sus::Vec<i32>(1, 2);
    auto bigg = sus::Vec<i32>(1, 1, 1);
    EXPECT_EQ(
        std::strong_ordering::less,
        smol.iter().strong_cmp_by(
            bigg.iter(), [](const i32& a, const i32& b) { return b <=> a; }));
  }
  {
    auto smol = sus::Vec<i32>(1, 2);
    auto bigg = sus::Vec<i32>(1, 2);
    EXPECT_EQ(
        std::strong_ordering::equal,
        smol.iter().strong_cmp_by(
            bigg.iter(), [](const i32& a, const i32& b) { return b <=> a; }));
  }

  // iter_mut.
  {
    auto smol = sus::Vec<i32>(1, 3);
    auto bigg = sus::Vec<i32>(1, 2);
    EXPECT_EQ(std::strong_ordering::less,
              smol.iter_mut().strong_cmp_by(
                  bigg.iter_mut(),
                  [](const i32& a, const i32& b) { return b <=> a; }));
  }

  // into_iter.
  {
    auto smol = sus::Vec<i32>(1, 3);
    auto bigg = sus::Vec<i32>(1, 2);
    EXPECT_EQ(std::strong_ordering::less,
              sus::move(smol).into_iter().strong_cmp_by(
                  sus::move(bigg).into_iter(),
                  [](const i32& a, const i32& b) { return b <=> a; }));
  }

  // Comparable but different types.
  {
    auto one = sus::Vec<i32>(1, 2);
    auto two = sus::Vec<i64>(1, 3);
    EXPECT_EQ(
        std::strong_ordering::greater,
        one.iter().strong_cmp_by(
            two.iter(), [](const i32& a, const i64& b) { return b <=> a; }));
  }

  static_assert(sus::Array<i32, 2>(1, 2).into_iter().strong_cmp_by(
                    sus::Array<i32, 2>(1, 3), [](const i32& a, const i32& b) {
                      return b <=> a;
                    }) == std::strong_ordering::greater);
}

TEST(Iterator, PartialCmp) {
  {
    auto smol = sus::Array<f32, 2>(1.f, 2.f);
    auto bigg = sus::Array<f32, 3>(1.f, 2.f, 1.f);
    EXPECT_EQ(std::partial_ordering::less,
              smol.iter().partial_cmp(bigg.iter()));
  }
  {
    auto bigg = sus::Array<f32, 3>(1.f, 2.f, 1.f);
    auto smol = sus::Array<f32, 2>(1.f, 2.f);
    EXPECT_EQ(std::partial_ordering::greater,
              bigg.iter().partial_cmp(smol.iter()));
  }
  {
    auto smol = sus::Array<f32, 2>(1.f, 2.f);
    auto bigg = sus::Array<f32, 3>(1.f, 1.f, 1.f);
    EXPECT_EQ(std::partial_ordering::greater,
              smol.iter().partial_cmp(bigg.iter()));
  }
  {
    auto smol = sus::Array<f32, 2>(1.f, 2.f);
    auto bigg = sus::Array<f32, 2>(1.f, 2.f);
    EXPECT_EQ(std::partial_ordering::equivalent,
              smol.iter().partial_cmp(bigg.iter()));
  }
  {
    auto smol = sus::Array<f32, 2>(f32::NaN, f32::NaN);
    auto bigg = sus::Array<f32, 2>(f32::NaN, f32::NaN);
    EXPECT_EQ(std::partial_ordering::unordered,
              smol.iter().partial_cmp(bigg.iter()));
  }
  {
    auto smol = sus::Array<f32, 2>(f32::NaN, 1.f);
    auto bigg = sus::Array<f32, 2>(f32::NaN, 2.f);
    EXPECT_EQ(std::partial_ordering::unordered,
              smol.iter().partial_cmp(bigg.iter()));
  }
  {
    auto smol = sus::Array<f32, 2>(1.f, f32::NaN);
    auto bigg = sus::Array<f32, 2>(f32::NaN, f32::NaN);
    EXPECT_EQ(std::partial_ordering::unordered,
              smol.iter().partial_cmp(bigg.iter()));
  }
  {
    auto smol = sus::Array<f32, 2>(1.f, f32::NaN);
    auto bigg = sus::Array<f32, 2>(2.f, f32::NaN);
    EXPECT_EQ(std::partial_ordering::less,
              smol.iter().partial_cmp(bigg.iter()));
  }

  // iter_mut.
  {
    auto smol = sus::Array<f32, 2>(1.f, 3.f);
    auto bigg = sus::Array<f32, 2>(1.f, 2.f);
    EXPECT_EQ(std::partial_ordering::greater,
              smol.iter_mut().partial_cmp(bigg.iter_mut()));
  }

  // into_iter.
  {
    auto smol = sus::Array<f32, 2>(1.f, 3.f);
    auto bigg = sus::Array<f32, 2>(1.f, 2.f);
    EXPECT_EQ(
        std::partial_ordering::greater,
        sus::move(smol).into_iter().partial_cmp(sus::move(bigg).into_iter()));
  }

  // Comparable but different types.
  {
    auto one = sus::Array<f32, 2>(1.f, 2.f);
    auto two = sus::Array<f64, 2>(1., 3.);
    EXPECT_EQ(std::partial_ordering::less, one.iter().partial_cmp(two.iter()));
  }

  static_assert(sus::Array<f32, 2>(1.f, 2.f).into_iter().partial_cmp(
                    sus::Array<f32, 2>(1.f, 3.f)) ==
                std::partial_ordering::less);
  static_assert(sus::Array<f32, 3>(1.f, 2.f, 0.f)
                    .into_iter()
                    .partial_cmp(sus::Array<f32, 2>(1.f, 2.f)) ==
                std::partial_ordering::greater);
}

TEST(Iterator, PartialCmpBy) {
  {
    auto smol = sus::Array<f32, 2>(1.f, 2.f);
    auto bigg = sus::Array<f32, 3>(1.f, 2.f, 1.f);
    EXPECT_EQ(
        std::partial_ordering::less,
        smol.iter().partial_cmp_by(
            bigg.iter(), [](const f32& a, const f32& b) { return b <=> a; }));
  }
  {
    auto bigg = sus::Array<f32, 3>(1.f, 2.f, 1.f);
    auto smol = sus::Array<f32, 2>(1.f, 2.f);
    EXPECT_EQ(
        std::partial_ordering::greater,
        bigg.iter().partial_cmp_by(
            smol.iter(), [](const f32& a, const f32& b) { return b <=> a; }));
  }
  {
    auto smol = sus::Array<f32, 2>(1.f, 2.f);
    auto bigg = sus::Array<f32, 3>(1.f, 1.f, 1.f);
    EXPECT_EQ(
        std::partial_ordering::less,
        smol.iter().partial_cmp_by(
            bigg.iter(), [](const f32& a, const f32& b) { return b <=> a; }));
  }
  {
    auto smol = sus::Array<f32, 2>(1.f, 2.f);
    auto bigg = sus::Array<f32, 2>(1.f, 2.f);
    EXPECT_EQ(
        std::partial_ordering::equivalent,
        smol.iter().partial_cmp_by(
            bigg.iter(), [](const f32& a, const f32& b) { return b <=> a; }));
  }

  // iter_mut.
  {
    auto smol = sus::Array<f32, 2>(1.f, 3.f);
    auto bigg = sus::Array<f32, 2>(1.f, 2.f);
    EXPECT_EQ(std::partial_ordering::less,
              smol.iter_mut().partial_cmp_by(
                  bigg.iter_mut(),
                  [](const f32& a, const f32& b) { return b <=> a; }));
  }

  // into_iter.
  {
    auto smol = sus::Array<f32, 2>(1.f, 3.f);
    auto bigg = sus::Array<f32, 2>(1.f, 2.f);
    EXPECT_EQ(std::partial_ordering::less,
              sus::move(smol).into_iter().partial_cmp_by(
                  sus::move(bigg).into_iter(),
                  [](const f32& a, const f32& b) { return b <=> a; }));
  }

  // Comparable but different types.
  {
    auto one = sus::Array<f32, 2>(1.f, 2.f);
    auto two = sus::Array<f64, 2>(1., 3.);
    EXPECT_EQ(
        std::partial_ordering::greater,
        one.iter().partial_cmp_by(
            two.iter(), [](const f32& a, const f64& b) { return b <=> a; }));
  }

  static_assert(sus::Array<f32, 2>(1.f, 2.f).into_iter().partial_cmp_by(
                    sus::Array<f32, 2>(1.f, 3.f),
                    [](const f32& a, const f32& b) { return b <=> a; }) ==
                std::partial_ordering::greater);
}

template <sus::mem::Copy T>
  requires(sus::cmp::StrongOrd<T> && sus::cmp::Eq<T>)
struct WeakT {
  sus_clang_bug_54040(constexpr inline WeakT(T a, T b) : a(a), b(b){});

  template <class U>
    requires(sus::cmp::Eq<U>)
  constexpr auto operator==(const WeakT<U>& o) const& noexcept {
    return a == o.a && b == o.b;
  }
  template <class U>
    requires(sus::cmp::Ord<U>)
  constexpr std::weak_ordering operator<=>(const WeakT<U>& o) const& noexcept {
    if ((a <=> o.a) == 0) return std::weak_ordering::equivalent;
    if ((a <=> o.a) < 0) return std::weak_ordering::less;
    return std::weak_ordering::greater;
  }

  T a;
  T b;
};

using Weak = WeakT<i32>;

TEST(Iterator, Cmp) {
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2), Weak(1, 1));
    EXPECT_EQ(std::weak_ordering::less, smol.iter().cmp(bigg.iter()));
  }
  {
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2), Weak(1, 1));
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::greater, bigg.iter().cmp(smol.iter()));
  }
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::equivalent, smol.iter().cmp(bigg.iter()));
  }
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(2, 1));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::equivalent, smol.iter().cmp(bigg.iter()));
  }
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(1, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::less, smol.iter().cmp(bigg.iter()));
  }

  // iter_mut.
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(3, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::greater,
              smol.iter_mut().cmp(bigg.iter_mut()));
  }

  // into_iter.
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(3, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::greater,
              sus::move(smol).into_iter().cmp(sus::move(bigg).into_iter()));
  }

  // Comparable but different types.
  {
    auto one = sus::Vec<WeakT<i32>>(WeakT<i32>(1, 1), WeakT<i32>(2, 2));
    auto two = sus::Vec<WeakT<i64>>(WeakT<i64>(1, 1), WeakT<i64>(3, 2));
    EXPECT_EQ(std::weak_ordering::less, one.iter().cmp(two.iter()));
  }

  // Strong ordering.
  {
    auto smol = sus::Vec<i32>(1, 2);
    auto bigg = sus::Vec<i32>(1, 2, 1);
    EXPECT_EQ(std::weak_ordering::less, smol.iter().cmp(bigg.iter()));
  }

  static_assert(sus::Vec<Weak>(Weak(1, 1), Weak(3, 2))
                    .into_iter()
                    .cmp(sus::Vec<Weak>(Weak(1, 1), Weak(2, 2))) ==
                std::weak_ordering::greater);
}

TEST(Iterator, CmpBy) {
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2), Weak(1, 1));
    EXPECT_EQ(std::weak_ordering::less,
              smol.iter().cmp_by(bigg.iter(), [](const Weak& a, const Weak& b) {
                return b <=> a;
              }));
  }
  {
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2), Weak(1, 1));
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::greater,
              bigg.iter().cmp_by(smol.iter(), [](const Weak& a, const Weak& b) {
                return b <=> a;
              }));
  }
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::equivalent,
              smol.iter().cmp_by(bigg.iter(), [](const Weak& a, const Weak& b) {
                return b <=> a;
              }));
  }
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(2, 1));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::equivalent,
              smol.iter().cmp_by(bigg.iter(), [](const Weak& a, const Weak& b) {
                return b <=> a;
              }));
  }
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(1, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::weak_ordering::greater,
              smol.iter().cmp_by(bigg.iter(), [](const Weak& a, const Weak& b) {
                return b <=> a;
              }));
  }

  // iter_mut.
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(3, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::partial_ordering::less,
              smol.iter_mut().cmp_by(
                  bigg.iter_mut(),
                  [](const Weak& a, const Weak& b) { return b <=> a; }));
  }

  // into_iter.
  {
    auto smol = sus::Vec<Weak>(Weak(1, 1), Weak(3, 2));
    auto bigg = sus::Vec<Weak>(Weak(1, 1), Weak(2, 2));
    EXPECT_EQ(std::partial_ordering::less,
              sus::move(smol).into_iter().cmp_by(
                  sus::move(bigg).into_iter(),
                  [](const Weak& a, const Weak& b) { return b <=> a; }));
  }

  // Comparable but different types.
  {
    auto one = sus::Vec<WeakT<i32>>(WeakT<i32>(1, 1), WeakT<i32>(2, 2));
    auto two = sus::Vec<WeakT<i64>>(WeakT<i64>(1, 1), WeakT<i64>(3, 2));
    EXPECT_EQ(std::weak_ordering::greater,
              one.iter().cmp_by(two.iter(),
                                [](const WeakT<i32>& a, const WeakT<i64>& b) {
                                  return b <=> a;
                                }));
  }

  // Strong ordering.
  {
    auto smol = sus::Vec<i32>(1, 2);
    auto bigg = sus::Vec<i32>(1, 2, 1);
    EXPECT_EQ(
        std::weak_ordering::less,
        smol.iter().cmp_by(bigg.iter(), [](i32 a, i32 b) { return b <=> a; }));
  }

  static_assert(sus::Vec<Weak>(Weak(1, 1), Weak(3, 2))
                    .into_iter()
                    .cmp_by(sus::Vec<Weak>(Weak(1, 1), Weak(2, 2)),
                            [](const Weak& a, const Weak& b) {
                              return b <=> a;
                            }) == std::weak_ordering::less);
}

TEST(Iterator, Eq) {
  struct E {
    i32 i;
    constexpr bool operator==(const E& rhs) const& noexcept {
      return i == rhs.i;
    }
  };
  static_assert(sus::cmp::Eq<E>);
  static_assert(!sus::cmp::StrongOrd<E>);

  {
    auto smol = sus::Vec<E>(E(1), E(2));
    auto bigg = sus::Vec<E>(E(1), E(2), E(1));
    EXPECT_EQ(false, smol.iter().eq(bigg.iter()));
  }
  {
    auto bigg = sus::Vec<E>(E(1), E(2), E(1));
    auto smol = sus::Vec<E>(E(1), E(2));
    EXPECT_EQ(false, bigg.iter().eq(smol.iter()));
  }
  {
    auto smol = sus::Vec<E>(E(1), E(2));
    auto bigg = sus::Vec<E>(E(1), E(1), E(1));
    EXPECT_EQ(false, smol.iter().eq(bigg.iter()));
  }
  {
    auto smol = sus::Vec<E>(E(1), E(2));
    auto bigg = sus::Vec<E>(E(1), E(2));
    EXPECT_EQ(true, smol.iter().eq(bigg.iter()));
  }

  // iter_mut.
  {
    auto smol = sus::Vec<E>(E(1), E(3));
    auto bigg = sus::Vec<E>(E(1), E(2));
    EXPECT_EQ(false, smol.iter_mut().eq(bigg.iter_mut()));
  }

  // into_iter.
  {
    auto smol = sus::Vec<E>(E(1), E(3));
    auto bigg = sus::Vec<E>(E(1), E(2));
    EXPECT_EQ(false,
              sus::move(smol).into_iter().eq(sus::move(bigg).into_iter()));
  }

  // Comparable but different types.
  {
    auto one = sus::Vec<i32>(1, 2);
    auto two = sus::Vec<i64>(1, 2);
    EXPECT_EQ(true, one.iter().eq(two.iter()));
  }

  static_assert(
      sus::Vec<i32>(2, 3, 4).into_iter().eq(sus::Array<i32, 3>(2, 3, 4)));
}

TEST(Iterator, EqBy) {
  struct E {
    i32 i;
    constexpr bool operator==(const E& rhs) const& noexcept {
      return i == rhs.i;
    }
  };
  static_assert(sus::cmp::Eq<E>);
  static_assert(!sus::cmp::StrongOrd<E>);

  {
    auto bigg = sus::Vec<E>(E(1), E(1), E(1));
    auto smol = sus::Vec<E>(E(2), E(2));
    EXPECT_EQ(false, smol.iter().eq_by(bigg.iter(), [](const E& a, const E& b) {
      return a.i + 1 == b.i;
    }));
  }
  {
    auto smol = sus::Vec<E>(E(1), E(1));
    auto bigg = sus::Vec<E>(E(2), E(2), E(1));
    EXPECT_EQ(false, bigg.iter().eq_by(smol.iter(), [](const E& a, const E& b) {
      return a.i + 1 == b.i;
    }));
  }
  {
    auto smol = sus::Vec<E>(E(1), E(1));
    auto bigg = sus::Vec<E>(E(2), E(3), E(1));
    EXPECT_EQ(false, smol.iter().eq_by(bigg.iter(), [](const E& a, const E& b) {
      return a.i + 1 == b.i;
    }));
  }
  {
    auto smol = sus::Vec<E>(E(1), E(1));
    auto bigg = sus::Vec<E>(E(2), E(2));
    EXPECT_EQ(true, smol.iter().eq_by(bigg.iter(), [](const E& a, const E& b) {
      return a.i + 1 == b.i;
    }));
  }

  // iter_mut.
  {
    auto smol = sus::Vec<E>(E(1), E(1));
    auto bigg = sus::Vec<E>(E(2), E(3));
    EXPECT_EQ(false, smol.iter_mut().eq_by(bigg.iter_mut(),
                                           [](const E& a, const E& b) {
                                             return a.i + 1 == b.i;
                                           }));
  }

  // into_iter.
  {
    auto smol = sus::Vec<E>(E(1), E(1));
    auto bigg = sus::Vec<E>(E(2), E(3));
    EXPECT_EQ(false,
              sus::move(smol).into_iter().eq_by(
                  sus::move(bigg).into_iter(),
                  [](const E& a, const E& b) { return a.i + 1 == b.i; }));
  }

  // Comparable but different types.
  {
    auto one = sus::Vec<i32>(1, 1);
    auto two = sus::Vec<i64>(2, 2);
    EXPECT_EQ(true,
              one.iter().eq_by(two.iter(), [](const i32& a, const i64& b) {
                return a + 1 == b;
              }));
  }

  static_assert(sus::Vec<i32>(2, 3, 4).into_iter().eq_by(
      sus::Array<i32, 3>(1, 2, 3), [](i32 a, i32 b) { return a == b + 1; }));
}

struct UnknownLimitIter final
    : public sus::iter::IteratorBase<UnknownLimitIter, i32> {
  using Item = i32;
  sus::Option<i32> next() noexcept { return sus::none(); }
  sus::iter::SizeHint size_hint() const noexcept {
    return sus::iter::SizeHint(0u, sus::some(1u));
  }

  sus_class_trivially_relocatable(unsafe_fn);
};
static_assert(sus::mem::Clone<UnknownLimitIter>);
static_assert(sus::iter::Iterator<UnknownLimitIter, i32>);
static_assert(sus::mem::TriviallyRelocatable<UnknownLimitIter>);

TEST(Iterator, Cycle) {
  // Empty.
  {
    auto v = sus::Vec<i32>();
    auto it = v.iter().cycle();
    static_assert(std::same_as<decltype(it.next()), sus::Option<const i32&>>);
    // Returns nothing.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.next(), sus::None);
  }
  // One.
  {
    auto v = sus::Vec<i32>(4);
    auto it = v.iter().cycle();
    static_assert(std::same_as<decltype(it.next()), sus::Option<const i32&>>);
    // No limit to the return.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(usize::MAX, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.next().unwrap(), 4);
  }
  // More.
  {
    auto v = sus::Vec<i32>(1, 2, 3);
    auto it = v.iter().cycle();
    static_assert(std::same_as<decltype(it.next()), sus::Option<const i32&>>);
    // No limit to the return.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(usize::MAX, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.next().unwrap(), 1);
  }

  // An iterator with a 0 lower bound and non-0 upper bound.
  {
    auto it = UnknownLimitIter().cycle();
    static_assert(std::same_as<decltype(it.next()), sus::Option<i32>>);
    // May return 0 or unlimited.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
  }

  static_assert(sus::Vec<i32>(2, 3, 4).into_iter().cycle().take(32u).count() ==
                32u);
}

TEST(Iterator, Find) {
  auto a = sus::Array<i32, 5>(1, 2, 3, 4, 5);

  // iter().
  {
    decltype(auto) s = a.iter().find([](const i32& i) { return i == 3; });
    static_assert(std::same_as<decltype(s), sus::Option<const i32&>>);
    EXPECT_EQ(s, sus::some(3_i32));

    decltype(auto) n = a.iter().find([](const i32& i) { return i == 0; });
    static_assert(std::same_as<decltype(n), sus::Option<const i32&>>);
    EXPECT_EQ(n, sus::None);

    // Repeated calls.
    auto it = a.iter();
    EXPECT_EQ(it.find([](const i32& i) { return i % 2 == 1; }).unwrap(), 1);
    EXPECT_EQ(it.find([](const i32& i) { return i % 2 == 1; }).unwrap(), 3);
  }
  // iter_mut().
  {
    decltype(auto) s = a.iter_mut().find([](const i32& i) { return i == 3; });
    static_assert(std::same_as<decltype(s), sus::Option<i32&>>);
    EXPECT_EQ(s, sus::some(a[2u]));

    decltype(auto) n = a.iter_mut().find([](const i32& i) { return i == 0; });
    static_assert(std::same_as<decltype(n), sus::Option<i32&>>);
    EXPECT_EQ(n, sus::None);

    // Repeated calls.
    auto it = a.iter_mut();
    EXPECT_EQ(it.find([](const i32& i) { return i % 2 == 1; }).unwrap(), 1);
    EXPECT_EQ(it.find([](const i32& i) { return i % 2 == 1; }).unwrap(), 3);
  }
  // into_iter().
  {
    decltype(auto) s =
        sus::clone(a).into_iter().find([](const i32& i) { return i == 3; });
    static_assert(std::same_as<decltype(s), sus::Option<i32>>);
    EXPECT_EQ(s, sus::some(3_i32));

    decltype(auto) n =
        sus::clone(a).into_iter().find([](const i32& i) { return i == 0; });
    static_assert(std::same_as<decltype(n), sus::Option<i32>>);
    EXPECT_EQ(n, sus::None);

    // Repeated calls.
    auto it = a.iter();
    EXPECT_EQ(it.find([](i32 i) { return i % 2 == 1; }).unwrap(), 1);
    EXPECT_EQ(it.find([](i32 i) { return i % 2 == 1; }).unwrap(), 3);
  }

  static_assert(sus::Vec<i32>(2, 3, 4, 3, 2)
                    .into_iter()
                    .enumerate()
                    .find([](auto p) { return p.template at<1>() == 3; })
                    .unwrap() == sus::Tuple<usize, i32>(1u, 3));
}

TEST(Iterator, FindMap) {
  auto a = sus::Array<i32, 5>(1, 2, 3, 4, 5);

  // iter().
  {
    decltype(auto) s = a.iter().find_map([](const i32& i) -> sus::Option<u32> {
      if (i == 3) {
        return sus::some(3u);
      } else {
        return sus::none();
      }
    });
    static_assert(std::same_as<decltype(s), sus::Option<u32>>);
    EXPECT_EQ(s, sus::some(3_u32));

    decltype(auto) n = a.iter().find_map(
        [](const i32&) -> sus::Option<u32> { return sus::none(); });
    static_assert(std::same_as<decltype(n), sus::Option<u32>>);
    EXPECT_EQ(n, sus::None);
  }
  // iter_mut().
  {
    decltype(auto) s = a.iter_mut().find_map([](i32& i) -> sus::Option<u32> {
      if (i == 3) {
        return sus::some(3u);
      } else {
        return sus::none();
      }
    });
    static_assert(std::same_as<decltype(s), sus::Option<u32>>);
    EXPECT_EQ(s, sus::some(3_u32));

    decltype(auto) n = a.iter_mut().find_map(
        [](i32&) -> sus::Option<u32> { return sus::none(); });
    static_assert(std::same_as<decltype(n), sus::Option<u32>>);
    EXPECT_EQ(n, sus::None);
  }
  // into_iter().
  {
    decltype(auto) s =
        sus::clone(a).into_iter().find_map([](i32&& i) -> sus::Option<u32> {
          if (i == 3) {
            return sus::some(3u);
          } else {
            return sus::none();
          }
        });
    static_assert(std::same_as<decltype(s), sus::Option<u32>>);
    EXPECT_EQ(s, sus::some(3_u32));

    decltype(auto) n = sus::clone(a).into_iter().find_map(
        [](i32&&) -> sus::Option<u32> { return sus::none(); });
    static_assert(std::same_as<decltype(n), sus::Option<u32>>);
    EXPECT_EQ(n, sus::None);
  }

  // Works on lvalue iterator.
  {
    auto find_even = [](const i32& i) -> sus::Option<u32> {
      if (i % 2 == 0) return u32::try_from(i).ok();
      return sus::none();
    };

    auto it = a.iter();
    // Finds the 2nd.
    auto r1 = it.find_map(find_even);
    EXPECT_EQ(r1, sus::some(2u));
    // Finds the 4th.
    auto r2 = it.find_map(find_even);
    EXPECT_EQ(r2, sus::some(4u));
    // Finds the end.
    auto r3 = it.find_map(find_even);
    EXPECT_EQ(r3, sus::none());
  }

  static_assert(sus::Vec<i32>(2, 3, 4, 3, 2)
                    .into_iter()
                    .enumerate()
                    .find_map([](auto p) -> sus::Option<u32> {
                      if (p.template at<1>() == 3) {
                        return sus::some(101u);
                      } else {
                        return sus::none();
                      }
                    }) == sus::some(101u));
  static_assert(sus::Vec<i32>(2, 3, 4, 3, 2)
                    .into_iter()
                    .find_map([](i32 i) -> sus::Option<u32> {
                      if (i == 99) {
                        return sus::some(101u);
                      } else {
                        return sus::none();
                      }
                    }) == sus::none());
}

TEST(Iterator, Flatten) {
  // By value/into_iter, forward.
  {
    auto vecs = sus::Vec<Vec<i32>>(sus::Vec<i32>(1, 2, 3),  //
                                   sus::Vec<i32>(4),        //
                                   sus::Vec<i32>(),         //
                                   sus::Vec<i32>(5, 6));
    auto it = sus::move(vecs).into_iter().flatten();
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 5);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 6);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // By value/into_iter, backward.
  {
    auto vecs = sus::Vec(sus::Vec<i32>(1, 2, 3),  //
                         sus::Vec<i32>(4),        //
                         sus::Vec<i32>(),         //
                         sus::Vec<i32>(5, 6),
                         sus::Vec<i32>()  //
    );
    auto it = sus::move(vecs).into_iter().flatten();
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 6);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 5);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back(), sus::None);
  }
  // By value/into_iter, backward with 1 left in the first iterator, then
  // forward.
  {
    auto vecs = sus::Vec<Vec<i32>>(sus::Vec<i32>(1, 2), sus::Vec<i32>(3, 4));
    auto it = sus::move(vecs).into_iter().flatten();
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // By value/into_iter, backward with none left in the first iterator, then
  // forward.
  {
    auto vecs = sus::Vec<Vec<i32>>(sus::Vec<i32>(1, 2), sus::Vec<i32>(3, 4));
    auto it = sus::move(vecs).into_iter().flatten();
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // By value/into_iter, forward with 1 left in the first iterator, then
  // backward.
  {
    auto vecs = sus::Vec<Vec<i32>>(sus::Vec<i32>(1, 2), sus::Vec<i32>(3, 4));
    auto it = sus::move(vecs).into_iter().flatten();
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // By value/into_iter, forward with none left in the first iterator, then
  // backward.
  {
    auto vecs = sus::Vec<Vec<i32>>(sus::Vec<i32>(1, 2), sus::Vec<i32>(3, 4));
    auto it = sus::move(vecs).into_iter().flatten();
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // iter().
  {
    auto v1 = sus::Vec<i32>(1, 2);
    auto v2 = sus::Vec<i32>(3);
    auto vecs = sus::Vec(v1.iter(), v2.iter());
    auto it = sus::move(vecs).into_iter().flatten();
    static_assert(std::same_as<decltype(it.next()), Option<const i32&>>);
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.next(), sus::None);
  }
  // iter_mut().
  {
    auto v1 = sus::Vec<i32>(1, 2);
    auto v2 = sus::Vec<i32>(3);
    auto vecs = sus::Vec(v1.iter_mut(), v2.iter_mut());
    auto it = sus::move(vecs).into_iter().flatten();
    static_assert(std::same_as<decltype(it.next()), Option<i32&>>);
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.next(), sus::None);
  }

  sus_gcc_bug_110905_else(                                      //
      static_assert(sus::Vec<Vec<i32>>(sus::Vec<i32>(1, 2, 3),  //
                                       sus::Vec<i32>(4),        //
                                       sus::Vec<i32>(),         //
                                       sus::Vec<i32>(5, 6))
                        .into_iter()
                        .flatten()
                        .sum() == 1 + 2 + 3 + 4 + 5 + 6)  //
  );
}

TEST(Iterator, FlatMap) {
  struct Integers {
    i32 a;
    i32 b;
    static auto make_iterable(Integers&& i) {
      return sus::Array<i32, 2>(i.a, i.b);
    }
  };

  // By value/into_iter, forward.
  {
    auto vec = sus::Vec<Integers>(  //
        Integers(1, 2), Integers(3, 4), Integers(10, 11));
    auto it = sus::move(vec).into_iter().flat_map(&Integers::make_iterable);
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 10);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 11);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // By value/into_iter, backward.
  {
    auto vec = sus::Vec<Integers>(  //
        Integers(1, 2), Integers(3, 4), Integers(10, 11));
    auto it = sus::move(vec).into_iter().flat_map(&Integers::make_iterable);
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 11);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 10);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back(), sus::None);
  }
  // By value/into_iter, backward with 1 left in the first iterator, then
  // forward.
  {
    auto vec = sus::Vec<Integers>(Integers(1, 2), Integers(3, 4));
    auto it = sus::move(vec).into_iter().flat_map(&Integers::make_iterable);
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // By value/into_iter, backward with none left in the first iterator, then
  // forward.
  {
    auto vec = sus::Vec<Integers>(Integers(1, 2), Integers(3, 4));
    auto it = sus::move(vec).into_iter().flat_map(&Integers::make_iterable);
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // By value/into_iter, forward with 1 left in the first iterator, then
  // backward.
  {
    auto vec = sus::Vec<Integers>(Integers(1, 2), Integers(3, 4));
    auto it = sus::move(vec).into_iter().flat_map(&Integers::make_iterable);
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  // By value/into_iter, forward with none left in the first iterator, then
  // backward.
  {
    auto vec = sus::Vec<Integers>(Integers(1, 2), Integers(3, 4));
    auto it = sus::move(vec).into_iter().flat_map(&Integers::make_iterable);
    static_assert(std::same_as<decltype(it.next()), Option<i32>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }

  static usize moves;
  struct MyIntoIter {
    MyIntoIter(i32 a, i32 b) noexcept : a(a), b(b) {}
    MyIntoIter(MyIntoIter&&) noexcept { moves += 1u; }
    MyIntoIter& operator=(MyIntoIter&&) noexcept { return moves += 1u, *this; }
    i32 a;
    i32 b;
    static MyIntoIter&& return_self(MyIntoIter&& i) noexcept {
      moves = 0u;
      return sus::move(i);
    }
    auto into_iter() && noexcept {
      return sus::Array<i32, 2>(a, b).into_iter();
    }
  };
  static_assert(sus::iter::IntoIterator<MyIntoIter, i32>);

  // Test that if the map function returns the same type (which is IntoIterator
  // itself) as an rvalue reference, that no move is invoked.
  {
    auto vec = sus::Vec<MyIntoIter>(MyIntoIter(1, 2), MyIntoIter(3, 4));
    auto it = sus::move(vec).into_iter().flat_map(&MyIntoIter::return_self);
    for (auto _ : sus::move(it)) {
      // The return_self() call resets `moves` and returns a reference. That
      // reference should not be used to construct a MyIntoIter, in order to
      // return the i32 for iterating.
      EXPECT_EQ(moves, 0u);
    }
  }

  // Mapping References.
  {
    auto n = NoCopyMove();
    auto o = sus::Option<const NoCopyMove&>(n);
    EXPECT_EQ(o.iter()
                  .flat_map([](const NoCopyMove&) {
                    return sus::Vec<i32>(1, 2, 3, 4);
                  })
                  .sum(),
              1 + 2 + 3 + 4);
  }

  sus_gcc_bug_110905_else(  //
      static_assert(
          sus::Vec<i32>(2, 3, 4)
              .into_iter()
              .flat_map([](i32 i) { return sus::Vec<i32>(i, i + 1, i + 2); })
              .sum() == (2 + 3 + 4) + (3 + 4 + 5) + (4 + 5 + 6))  //
  );
}

TEST(Iterator, Fold) {
  // Check the accumulator type can be different from the iterating type.
  {
    auto it = sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter();
    auto o = sus::move(it).fold(
        10_u32,
        // Receiving rvalue ensures the caller did move.
        [](u32 acc, i32&& v) { return u32::try_from(v).unwrap() + acc; });
    static_assert(std::same_as<u32, decltype(o)>);
    EXPECT_EQ(o, (5u + (4u + (3u + (2u + (1u + 10u))))));
  }
  // Check order of iteration.
  {
    auto it = sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter();
    auto o = sus::move(it).fold(
        10_i32,
        // Receiving by value shows it doesn't need to receive by reference.
        [](i32 acc, i32 v) { return v - acc; });
    static_assert(std::same_as<i32, decltype(o)>);
    EXPECT_EQ(o, (5 - (4 - (3 - (2 - (1 - 10))))));
  }

  static_assert(sus::Array<char, 5>('a', 'b', 'c', 'd', 'e')
                    .into_iter()
                    .fold(sus::Vec<char>(), [](sus::Vec<char> acc, char v) {
                      acc.push(v);
                      return acc;
                    }) == sus::Vec('a', 'b', 'c', 'd', 'e'));
}

TEST(Iterator, Fold_Example_References) {
  auto v = sus::Vec<i32>(1, 2, 3);
  i32 init;
  i32& out = v.iter_mut().fold<i32&>(  //
      init, [](i32&, i32& v) -> i32& { return v; });
  sus_check(&out == &v.last().unwrap());
}

TEST(Iterator, Rfold) {
  // Check the accumulator type can be different from the iterating type.
  {
    auto it = sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter();
    auto o = sus::move(it).rfold(
        10_u32,
        // Receiving rvalue ensures the caller did move.
        [](u32 acc, i32&& v) { return u32::try_from(v).unwrap() + acc; });
    static_assert(std::same_as<u32, decltype(o)>);
    EXPECT_EQ(o, (1u + (2u + (3u + (4u + (5u + 10u))))));
  }
  // Check order of iteration.
  {
    auto it = sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter();
    auto o = sus::move(it).rfold(
        10_i32,
        // Receiving by value shows it doesn't need to receive by reference.
        [](i32 acc, i32 v) { return v - acc; });
    static_assert(std::same_as<i32, decltype(o)>);
    EXPECT_EQ(o, (1 - (2 - (3 - (4 - (5 - 10))))));
  }

  static_assert(sus::Array<char, 5>('a', 'b', 'c', 'd', 'e')
                    .into_iter()
                    .rfold(sus::Vec<char>(), [](sus::Vec<char> acc, char v) {
                      acc.push(v);
                      return acc;
                    }) == sus::Vec('e', 'd', 'c', 'b', 'a'));
}

TEST(Iterator, ForEach) {
  {
    auto it = sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter();
    static_assert(std::is_void_v<decltype(sus::move(it).for_each([](i32) {}))>);

    sus::Vec<i32> seen;
    sus::move(it).for_each([&seen](i32 v) { seen.push(v); });
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4, 5}));
  }

  struct Movable {
    Movable(i32 i) : i(i) {}
    Movable(Movable&& m) : i(m.i) {}
    Movable& operator=(Movable&& m) { return i = m.i, *this; }
    i32 i;
  };
  static_assert(sus::mem::Move<Movable>);
  static_assert(!sus::mem::Copy<Movable>);
  {
    sus::Vec<i32> seen;
    sus::Array<Movable, 2>(Movable(1), Movable(2))
        .into_iter()
        .for_each([&seen](Movable m) { seen.push(m.i); });
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2}));
  }

  static_assert([]() {
    sus::Vec<char> acc;
    sus::Array<char, 5>('a', 'b', 'c', 'd', 'e')
        .into_iter()
        .for_each([&](char v) { acc.push(v); });
    return acc;
  }() == sus::Vec('a', 'b', 'c', 'd', 'e'));
}

TEST(Iterator, Fuse) {
  struct Alternate final : public IteratorBase<Alternate, i32> {
    using Item = i32;
    constexpr Option<Item> next() noexcept {
      state_ += 1;
      return state_ % 2 == 1 ? Option<Item>(state_) : Option<Item>();
    }
    constexpr sus::iter::SizeHint size_hint() const noexcept {
      return {state_ % 2 == 0 ? 1u : 0u, sus::none()};
    }

    i32 state_;
  };
  static_assert(sus::iter::Iterator<Alternate, i32>);

  {
    auto it = Alternate();
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
  }
  {
    auto it = Alternate().fuse();
    // The non-empty iterator's size hint.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::none()));
    EXPECT_EQ(it.next().unwrap(), 1);
    // The empty iterator's size hint.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::none()));
    EXPECT_EQ(it.next(), sus::None);
    // Fuse knows nothing can be returned now.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.next(), sus::None);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.next(), sus::None);
  }

  // Forward and back.
  auto f = sus::Vec<i32>(1, 2).into_iter().fuse();
  EXPECT_EQ(f.next(), sus::some(1_i32));
  EXPECT_EQ(f.next(), sus::some(2_i32));
  EXPECT_EQ(f.next(), sus::none());
  EXPECT_EQ(f.next_back(), sus::none());
  auto b = sus::Vec<i32>(1, 2).into_iter().fuse();
  EXPECT_EQ(b.next_back(), sus::some(2_i32));
  EXPECT_EQ(b.next_back(), sus::some(1_i32));
  EXPECT_EQ(b.next_back(), sus::none());
  EXPECT_EQ(b.next(), sus::none());

  static_assert(Alternate().fuse().count() == 1u);
}

TEST(Iterator, Ge) {
  {
    auto it1 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(true, sus::move(it1).ge(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(true, sus::move(it1).ge(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).ge(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, f32::NaN).into_iter();
    EXPECT_EQ(false, sus::move(it1).ge(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, f32::NaN).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).ge(sus::move(it2)));
  }

  static_assert(
      sus::Vec<i32>(2, 4, 4).into_iter().ge(sus::Array<i32, 3>(2, 3, 4)));
}

TEST(Iterator, Gt) {
  {
    auto it1 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).gt(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(true, sus::move(it1).gt(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).gt(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, f32::NaN).into_iter();
    EXPECT_EQ(false, sus::move(it1).gt(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, f32::NaN).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).gt(sus::move(it2)));
  }

  static_assert(
      sus::Vec<i32>(2, 4, 4).into_iter().gt(sus::Array<i32, 3>(2, 3, 4)));
}

TEST(Iterator, Le) {
  {
    auto it1 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(true, sus::move(it1).le(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).le(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    EXPECT_EQ(true, sus::move(it1).le(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, f32::NaN).into_iter();
    EXPECT_EQ(false, sus::move(it1).le(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, f32::NaN).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).le(sus::move(it2)));
  }

  static_assert(
      sus::Vec<i32>(2, 2, 4).into_iter().le(sus::Array<i32, 3>(2, 3, 4)));
}

TEST(Iterator, Lt) {
  {
    auto it1 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).lt(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).lt(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    EXPECT_EQ(true, sus::move(it1).lt(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, 4.f).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, f32::NaN).into_iter();
    EXPECT_EQ(false, sus::move(it1).lt(sus::move(it2)));
  }
  {
    auto it1 = sus::Array<f32, 2>(1.f, f32::NaN).into_iter();
    auto it2 = sus::Array<f32, 2>(1.f, 3.f).into_iter();
    EXPECT_EQ(false, sus::move(it1).lt(sus::move(it2)));
  }

  static_assert(
      sus::Vec<i32>(2, 2, 4).into_iter().lt(sus::Array<i32, 3>(2, 3, 4)));
}

TEST(Iterator, Inspect) {
  // By value, into_iter().
  {
    static sus::Vec<i32> seen;
    auto it =
        sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter().inspect([](const i32& v) {
          seen.push(v);
        });
    static_assert(std::same_as<Option<i32>, decltype(it.next())>);
    EXPECT_EQ(it.next(), sus::some(1));
    EXPECT_EQ(seen, sus::Slice<i32>::from({1}));
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2}));
    EXPECT_EQ(it.next(), sus::some(3));
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3}));
    EXPECT_EQ(it.next(), sus::some(4));
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4}));
    EXPECT_EQ(it.next(), sus::some(5));
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4, 5}));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4, 5}));
  }
  // By ref, iter().
  {
    static sus::Vec<i32> seen;
    auto a = sus::Array<i32, 5>(1, 2, 3, 4, 5);
    auto it = a.iter().inspect([](const i32& v) { seen.push(v); });
    static_assert(std::same_as<Option<const i32&>, decltype(it.next())>);
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1}));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2}));
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3}));
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4}));
    EXPECT_EQ(it.next().unwrap(), 5);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4, 5}));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4, 5}));
  }
  // By mut ref, iter_mut().
  {
    static sus::Vec<i32> seen;
    auto a = sus::Array<i32, 5>(1, 2, 3, 4, 5);
    auto it = a.iter_mut().inspect([](const i32& v) { seen.push(v); });
    static_assert(std::same_as<Option<i32&>, decltype(it.next())>);
    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1}));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2}));
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3}));
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4}));
    EXPECT_EQ(it.next().unwrap(), 5);
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4, 5}));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(seen, sus::Slice<i32>::from({1, 2, 3, 4, 5}));
  }

  static_assert([]() {
    sus::Vec<i32> seen;
    sus::Array<i32, 5>(1, 2, 3, 4, 5)
        .into_iter()
        .inspect([&](const i32& v) { seen.push(v); })
        .count();
    return seen;
  }() == sus::Vec(1, 2, 3, 4, 5));
}

TEST(Iterator, Last) {
  // into_iter().
  {
    decltype(auto) n = sus::Array<i32, 0>().into_iter().last();
    static_assert(std::same_as<decltype(n), Option<i32>>);
    EXPECT_EQ(n, sus::None);
    decltype(auto) s = sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter().last();
    static_assert(std::same_as<decltype(s), Option<i32>>);
    EXPECT_EQ(s, sus::some(5));
  }
  // iter().
  {
    auto az = sus::Array<i32, 0>();
    decltype(auto) n = az.iter().last();
    static_assert(std::same_as<decltype(n), Option<const i32&>>);
    EXPECT_EQ(n, sus::None);

    auto an = sus::Array<i32, 5>(1, 2, 3, 4, 5);
    decltype(auto) s = an.iter().last();
    static_assert(std::same_as<decltype(s), Option<const i32&>>);
    EXPECT_EQ(s.as_value(), 5);
  }

  static_assert(sus::Array<i32, 0>().into_iter().last() == sus::none());
  static_assert(sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter().last() ==
                sus::some(5));
}

TEST(Iterator, Max) {
  // 0 items.
  {
    decltype(auto) n = sus::Array<i32, 0>().into_iter().max();
    static_assert(std::same_as<decltype(n), Option<i32>>);
    EXPECT_EQ(n, sus::None);
  }
  // 1 item.
  {
    decltype(auto) n = sus::Array<i32, 1>(3).into_iter().max();
    EXPECT_EQ(n.as_value(), 3);
  }
  // More items.
  {
    decltype(auto) n = sus::Array<i32, 3>(3, 2, 1).into_iter().max();
    EXPECT_EQ(n.as_value(), 3);
  }
  {
    decltype(auto) n = sus::Array<i32, 3>(1, 2, 3).into_iter().max();
    EXPECT_EQ(n.as_value(), 3);
  }
  {
    decltype(auto) n = sus::Array<i32, 3>(1, 3, 1).into_iter().max();
    EXPECT_EQ(n.as_value(), 3);
  }
  {
    decltype(auto) n = sus::Array<i32, 3>(3, 3, 1).into_iter().max();
    EXPECT_EQ(n.as_value(), 3);
  }

  // The last max value is returned.
  struct S {
    i32 i;
    i32 id;
    std::weak_ordering operator<=>(const S& b) const noexcept {
      return i <=> b.i;
    }
  };
  {
    decltype(auto) n =
        sus::Array<S, 3>(S(3, 0), S(3, 1), S(1, 2)).into_iter().max();
    EXPECT_EQ(n.as_value().id, 1);
  }
  {
    decltype(auto) n =
        sus::Array<S, 3>(S(3, 0), S(1, 1), S(3, 2)).into_iter().max();
    EXPECT_EQ(n.as_value().id, 2);
  }

  // iter().
  {
    auto a = sus::Array<i32, 3>(1, 3, 1);
    decltype(auto) n = a.iter().max();
    static_assert(std::same_as<decltype(n), Option<const i32&>>);
    EXPECT_EQ(n.as_value(), 3);
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 3>(1, 3, 1);
    decltype(auto) n = a.iter_mut().max();
    static_assert(std::same_as<decltype(n), Option<i32&>>);
    EXPECT_EQ(n.as_value(), 3);
  }

  static_assert(sus::Vec<i32>(2, 3, 2).into_iter().max() == sus::some(3));
  static_assert(sus::Vec<i32>().into_iter().max() == sus::none());
}

TEST(Iterator, MaxBy) {
  struct M {
    static auto cmp(const M& a, const M& b) { return a.i <=> b.i; }

    i32 i;
  };

  // 0 items.
  {
    decltype(auto) n = sus::Array<M, 0>().into_iter().max_by(&M::cmp);
    static_assert(std::same_as<decltype(n), Option<M>>);
    EXPECT_EQ(n, sus::None);
  }
  // 1 item.
  {
    decltype(auto) n = sus::Array<M, 1>(M(3)).into_iter().max_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 3);
  }
  // More items.
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(3), M(2), M(1)).into_iter().max_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 3);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(1), M(2), M(3)).into_iter().max_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 3);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(1), M(3), M(1)).into_iter().max_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 3);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(3), M(3), M(1)).into_iter().max_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 3);
  }

  // The last max value is returned.
  struct S {
    i32 i;
    i32 id;
    static auto cmp(const S& a, const S& b) noexcept { return a.i <=> b.i; }
  };
  {
    decltype(auto) n =
        sus::Array<S, 3>(S(3, 0), S(3, 1), S(1, 2)).into_iter().max_by(&S::cmp);
    EXPECT_EQ(n.as_value().id, 1);
  }
  {
    decltype(auto) n =
        sus::Array<S, 3>(S(3, 0), S(1, 1), S(3, 2)).into_iter().max_by(&S::cmp);
    EXPECT_EQ(n.as_value().id, 2);
  }

  // iter().
  {
    auto a = sus::Array<M, 3>(M(1), M(3), M(1));
    decltype(auto) n = a.iter().max_by(&M::cmp);
    static_assert(std::same_as<decltype(n), Option<const M&>>);
    EXPECT_EQ(n.as_value().i, 3);
  }
  // iter_mut().
  {
    auto a = sus::Array<M, 3>(M(1), M(3), M(1));
    decltype(auto) n = a.iter_mut().max_by(&M::cmp);
    static_assert(std::same_as<decltype(n), Option<M&>>);
    EXPECT_EQ(n.as_value().i, 3);
  }

  static_assert(
      sus::Vec<f32>(2.f, 3.f, 2.f).into_iter().max_by(&f32::total_cmp) ==
      sus::some(3.f));
  static_assert(sus::Vec<f32>().into_iter().max_by(&f32::total_cmp) ==
                sus::none());
}

TEST(Iterator, MaxByKey) {
  struct M {
    constexpr static i32 key(const M& m) { return m.i; }

    i32 i;
  };

  // 0 items.
  {
    decltype(auto) n = sus::Array<M, 0>().into_iter().max_by_key(&M::key);
    static_assert(std::same_as<decltype(n), Option<M>>);
    EXPECT_EQ(n, sus::None);
  }
  // 1 item.
  {
    decltype(auto) n = sus::Array<M, 1>(M(3)).into_iter().max_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 3);
  }
  // More items.
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(3), M(2), M(1)).into_iter().max_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 3);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(1), M(2), M(3)).into_iter().max_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 3);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(1), M(3), M(1)).into_iter().max_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 3);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(3), M(3), M(1)).into_iter().max_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 3);
  }

  // The last max value is returned.
  struct S {
    i32 i;
    i32 id;
    static i32 key(const S& s) noexcept { return s.i; }
  };
  {
    decltype(auto) n = sus::Array<S, 3>(S(3, 0), S(3, 1), S(1, 2))
                           .into_iter()
                           .max_by_key(&S::key);
    EXPECT_EQ(n.as_value().id, 1);
  }
  {
    decltype(auto) n = sus::Array<S, 3>(S(3, 0), S(1, 1), S(3, 2))
                           .into_iter()
                           .max_by_key(&S::key);
    EXPECT_EQ(n.as_value().id, 2);
  }

  // iter().
  {
    auto a = sus::Array<M, 3>(M(1), M(3), M(1));
    decltype(auto) n = a.iter().max_by_key(&M::key);
    static_assert(std::same_as<decltype(n), Option<const M&>>);
    EXPECT_EQ(n.as_value().i, 3);
  }
  // iter_mut().
  {
    auto a = sus::Array<M, 3>(M(1), M(3), M(1));
    decltype(auto) n = a.iter_mut().max_by_key(&M::key);
    static_assert(std::same_as<decltype(n), Option<M&>>);
    EXPECT_EQ(n.as_value().i, 3);
  }

  static_assert(sus::Vec<M>(M(2), M(3), M(2))
                    .into_iter()
                    .max_by_key(&M::key)
                    .unwrap()
                    .i == 3);
  static_assert(sus::Vec<M>().into_iter().max_by_key(&M::key).is_none());
}

TEST(Iterator, Min) {
  // 0 items.
  {
    decltype(auto) n = sus::Array<i32, 0>().into_iter().min();
    static_assert(std::same_as<decltype(n), Option<i32>>);
    EXPECT_EQ(n, sus::None);
  }
  // 1 item.
  {
    decltype(auto) n = sus::Array<i32, 1>(3).into_iter().min();
    EXPECT_EQ(n.as_value(), 3);
  }
  // More items.
  {
    decltype(auto) n = sus::Array<i32, 3>(3, 2, 1).into_iter().min();
    EXPECT_EQ(n.as_value(), 1);
  }
  {
    decltype(auto) n = sus::Array<i32, 3>(1, 2, 3).into_iter().min();
    EXPECT_EQ(n.as_value(), 1);
  }
  {
    decltype(auto) n = sus::Array<i32, 3>(3, 1, 3).into_iter().min();
    EXPECT_EQ(n.as_value(), 1);
  }
  {
    decltype(auto) n = sus::Array<i32, 3>(1, 1, 3).into_iter().min();
    EXPECT_EQ(n.as_value(), 1);
  }

  // The first min value is returned.
  struct S {
    i32 i;
    i32 id;
    std::weak_ordering operator<=>(const S& b) const noexcept {
      return i <=> b.i;
    }
  };
  {
    decltype(auto) n =
        sus::Array<S, 3>(S(1, 0), S(1, 1), S(3, 2)).into_iter().min();
    EXPECT_EQ(n.as_value().id, 0);
  }
  {
    decltype(auto) n =
        sus::Array<S, 3>(S(3, 0), S(1, 1), S(1, 2)).into_iter().min();
    EXPECT_EQ(n.as_value().id, 1);
  }

  // iter().
  {
    auto a = sus::Array<i32, 3>(3, 1, 3);
    decltype(auto) n = a.iter().min();
    static_assert(std::same_as<decltype(n), Option<const i32&>>);
    EXPECT_EQ(n.as_value(), 1);
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 3>(3, 1, 3);
    decltype(auto) n = a.iter_mut().min();
    static_assert(std::same_as<decltype(n), Option<i32&>>);
    EXPECT_EQ(n.as_value(), 1);
  }

  static_assert(sus::Vec<i32>(4, 3, 4).into_iter().min() == sus::some(3));
  static_assert(sus::Vec<i32>().into_iter().min() == sus::none());
}

TEST(Iterator, MinBy) {
  struct M {
    static auto cmp(const M& a, const M& b) { return a.i <=> b.i; }

    i32 i;
  };

  // 0 items.
  {
    decltype(auto) n = sus::Array<M, 0>().into_iter().min_by(&M::cmp);
    static_assert(std::same_as<decltype(n), Option<M>>);
    EXPECT_EQ(n, sus::None);
  }
  // 1 item.
  {
    decltype(auto) n = sus::Array<M, 1>(M(3)).into_iter().min_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 3);
  }
  // More items.
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(3), M(2), M(1)).into_iter().min_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 1);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(1), M(2), M(3)).into_iter().min_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 1);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(3), M(1), M(3)).into_iter().min_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 1);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(1), M(1), M(3)).into_iter().min_by(&M::cmp);
    EXPECT_EQ(n.as_value().i, 1);
  }

  // The first min value is returned.
  struct S {
    i32 i;
    i32 id;
    static auto cmp(const S& a, const S& b) noexcept { return a.i <=> b.i; }
  };
  {
    decltype(auto) n =
        sus::Array<S, 3>(S(3, 0), S(1, 1), S(1, 2)).into_iter().min_by(&S::cmp);
    EXPECT_EQ(n.as_value().id, 1);
  }
  {
    decltype(auto) n =
        sus::Array<S, 3>(S(1, 0), S(3, 1), S(1, 2)).into_iter().min_by(&S::cmp);
    EXPECT_EQ(n.as_value().id, 0);
  }

  // iter().
  {
    auto a = sus::Array<M, 3>(M(3), M(1), M(3));
    decltype(auto) n = a.iter().min_by(&M::cmp);
    static_assert(std::same_as<decltype(n), Option<const M&>>);
    EXPECT_EQ(&n.as_value(), &a[1u]);
    EXPECT_EQ(n.as_value().i, 1);
  }
  // iter_mut().
  {
    auto a = sus::Array<M, 3>(M(3), M(1), M(3));
    decltype(auto) n = a.iter_mut().min_by(&M::cmp);
    static_assert(std::same_as<decltype(n), Option<M&>>);
    EXPECT_EQ(&n.as_value(), &a[1u]);
    EXPECT_EQ(n.as_value().i, 1);
  }

  static_assert(
      sus::Vec<f32>(4.f, 3.f, 4.f).into_iter().min_by(&f32::total_cmp) ==
      sus::some(3.f));
  static_assert(sus::Vec<f32>().into_iter().min_by(&f32::total_cmp) ==
                sus::none());
}

TEST(Iterator, MinByKey) {
  struct M {
    constexpr static i32 key(const M& m) { return m.i; }

    i32 i;
  };

  // 0 items.
  {
    decltype(auto) n = sus::Array<M, 0>().into_iter().min_by_key(&M::key);
    static_assert(std::same_as<decltype(n), Option<M>>);
    EXPECT_EQ(n, sus::None);
  }
  // 1 item.
  {
    decltype(auto) n = sus::Array<M, 1>(M(3)).into_iter().min_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 3);
  }
  // More items.
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(3), M(2), M(1)).into_iter().min_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 1);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(1), M(2), M(3)).into_iter().min_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 1);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(3), M(1), M(3)).into_iter().min_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 1);
  }
  {
    decltype(auto) n =
        sus::Array<M, 3>(M(1), M(1), M(3)).into_iter().min_by_key(&M::key);
    EXPECT_EQ(n.as_value().i, 1);
  }

  // The first min value is returned.
  struct S {
    i32 i;
    i32 id;
    static i32 key(const S& s) noexcept { return s.i; }
  };
  {
    decltype(auto) n = sus::Array<S, 3>(S(1, 0), S(1, 1), S(3, 2))
                           .into_iter()
                           .min_by_key(&S::key);
    EXPECT_EQ(n.as_value().id, 0);
  }
  {
    decltype(auto) n = sus::Array<S, 3>(S(3, 0), S(1, 1), S(1, 2))
                           .into_iter()
                           .min_by_key(&S::key);
    EXPECT_EQ(n.as_value().id, 1);
  }

  // iter().
  {
    auto a = sus::Array<M, 3>(M(3), M(1), M(3));
    decltype(auto) n = a.iter().min_by_key(&M::key);
    static_assert(std::same_as<decltype(n), Option<const M&>>);
    EXPECT_EQ(n.as_value().i, 1);
  }
  // iter_mut().
  {
    auto a = sus::Array<M, 3>(M(3), M(1), M(3));
    decltype(auto) n = a.iter_mut().min_by_key(&M::key);
    static_assert(std::same_as<decltype(n), Option<M&>>);
    EXPECT_EQ(n.as_value().i, 1);
  }

  static_assert(sus::Vec<M>(M(4), M(3), M(4))
                    .into_iter()
                    .min_by_key(&M::key)
                    .unwrap()
                    .i == 3);
  static_assert(sus::Vec<M>().into_iter().min_by_key(&M::key).is_none());
}

TEST(Iterator, Ne) {
  struct E {
    i32 i;
    constexpr bool operator==(const E& rhs) const& noexcept {
      return i == rhs.i;
    }
  };
  static_assert(sus::cmp::Eq<E>);
  static_assert(!sus::cmp::StrongOrd<E>);

  {
    auto smol = sus::Vec<E>(E(1), E(2));
    auto bigg = sus::Vec<E>(E(1), E(2), E(1));
    EXPECT_EQ(true, smol.iter().ne(bigg.iter()));
  }
  {
    auto bigg = sus::Vec<E>(E(1), E(2), E(1));
    auto smol = sus::Vec<E>(E(1), E(2));
    EXPECT_EQ(true, bigg.iter().ne(smol.iter()));
  }
  {
    auto smol = sus::Vec<E>(E(1), E(2));
    auto bigg = sus::Vec<E>(E(1), E(1), E(1));
    EXPECT_EQ(true, smol.iter().ne(bigg.iter()));
  }
  {
    auto smol = sus::Vec<E>(E(1), E(2));
    auto bigg = sus::Vec<E>(E(1), E(2));
    EXPECT_EQ(false, smol.iter().ne(bigg.iter()));
  }

  // iter_mut.
  {
    auto smol = sus::Vec<E>(E(1), E(3));
    auto bigg = sus::Vec<E>(E(1), E(2));
    EXPECT_EQ(true, smol.iter_mut().ne(bigg.iter_mut()));
  }

  // into_iter.
  {
    auto smol = sus::Vec<E>(E(1), E(3));
    auto bigg = sus::Vec<E>(E(1), E(2));
    EXPECT_EQ(true,
              sus::move(smol).into_iter().ne(sus::move(bigg).into_iter()));
  }

  // Comparable but different types.
  {
    auto one = sus::Vec<i32>(1, 2);
    auto two = sus::Vec<i64>(1, 2);
    EXPECT_EQ(false, one.iter().ne(two.iter()));
  }

  static_assert(
      sus::Vec<i32>(2, 3, 4).into_iter().ne(sus::Array<i32, 3>(2, 3, 5)));
}

TEST(Iterator, Nth) {
  {
    auto it = sus::Array<i32, 0>().into_iter();
    EXPECT_EQ(it.nth(10u), sus::None);
  }
  {
    auto a = sus::Array<i32, 5>(1, 2, 3, 4, 5);
    auto it = a.iter();
    static_assert(std::same_as<decltype(it.next()), Option<const i32&>>);
    EXPECT_EQ(it.nth(0u).unwrap(), 1);
    EXPECT_EQ(it.nth(0u).unwrap(), 2);
    EXPECT_EQ(it.nth(2u).unwrap(), 5);
    EXPECT_EQ(it.nth(2u), sus::None);
  }

  static_assert(sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter().nth(3u) ==
                sus::some(4));
  static_assert(sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter().nth(4u) ==
                sus::some(5));
  static_assert(sus::Array<i32, 5>(1, 2, 3, 4, 5).into_iter().nth(5u) ==
                sus::none());
}

TEST(Iterator, NthBack) {
  {
    auto it = sus::Array<i32, 0>().into_iter();
    EXPECT_EQ(it.nth_back(10u), sus::None);
  }
  {
    auto a = sus::Array<i32, 5>(1, 2, 3, 4, 5);
    auto it = a.iter();
    static_assert(std::same_as<decltype(it.next()), Option<const i32&>>);
    EXPECT_EQ(it.nth_back(0u).unwrap(), 5);
    EXPECT_EQ(it.nth_back(0u).unwrap(), 4);
    EXPECT_EQ(it.nth_back(2u).unwrap(), 1);
    EXPECT_EQ(it.nth_back(2u), sus::None);
  }
}

TEST(Iterator, Rfind) {
  auto a = sus::Array<i32, 5>(1, 2, 3, 4, 5);

  // iter().
  {
    decltype(auto) s = a.iter().rfind([](const i32& i) { return i == 3; });
    static_assert(std::same_as<decltype(s), sus::Option<const i32&>>);
    EXPECT_EQ(s, sus::some(3_i32));

    decltype(auto) n = a.iter().rfind([](const i32& i) { return i == 0; });
    static_assert(std::same_as<decltype(n), sus::Option<const i32&>>);
    EXPECT_EQ(n, sus::None);

    // Repeated calls.
    auto it = a.iter();
    EXPECT_EQ(it.rfind([](const i32& i) { return i % 2 == 1; }).unwrap(), 5);
    EXPECT_EQ(it.rfind([](const i32& i) { return i % 2 == 1; }).unwrap(), 3);
  }
  // iter_mut().
  {
    decltype(auto) s = a.iter_mut().rfind([](const i32& i) { return i == 3; });
    static_assert(std::same_as<decltype(s), sus::Option<i32&>>);
    EXPECT_EQ(s, sus::some(a[2u]));

    decltype(auto) n = a.iter_mut().rfind([](const i32& i) { return i == 0; });
    static_assert(std::same_as<decltype(n), sus::Option<i32&>>);
    EXPECT_EQ(n, sus::None);

    // Repeated calls.
    auto it = a.iter_mut();
    EXPECT_EQ(it.rfind([](const i32& i) { return i % 2 == 1; }).unwrap(), 5);
    EXPECT_EQ(it.rfind([](const i32& i) { return i % 2 == 1; }).unwrap(), 3);
  }
  // into_iter().
  {
    decltype(auto) s =
        sus::clone(a).into_iter().rfind([](const i32& i) { return i == 3; });
    static_assert(std::same_as<decltype(s), sus::Option<i32>>);
    EXPECT_EQ(s, sus::some(3_i32));

    decltype(auto) n =
        sus::clone(a).into_iter().rfind([](const i32& i) { return i == 0; });
    static_assert(std::same_as<decltype(n), sus::Option<i32>>);
    EXPECT_EQ(n, sus::None);

    // Repeated calls.
    auto it = a.iter();
    EXPECT_EQ(it.rfind([](i32 i) { return i % 2 == 1; }).unwrap(), 5);
    EXPECT_EQ(it.rfind([](i32 i) { return i % 2 == 1; }).unwrap(), 3);
  }

  static_assert(sus::Vec<i32>(2, 5, 4, 5, 2)
                    .into_iter()
                    .enumerate()
                    .rfind([](auto p) { return p.template at<1>() == 5; })
                    .unwrap() == sus::Tuple<usize, i32>(3u, 5));
}

struct Extendable {
  constexpr void extend(sus::iter::IntoIterator<i32> auto in) {
    for (i32 each : sus::move(in).into_iter()) extend_sum += each;
  }
  i32 extend_sum;
};
static_assert(sus::construct::Default<Extendable>);
static_assert(sus::iter::Extend<Extendable, i32>);

TEST(Iterator, Partition) {
  // Partition from iter() for a Copy type.
  {
    auto a = sus::Array<i32, 5>(1, 2, 3, 4, 5);
    auto p = a.iter().partition<sus::Vec<i32>>([](const i32&) { return true; });
    static_assert(
        std::same_as<decltype(p), sus::Tuple<sus::Vec<i32>, sus::Vec<i32>>>);

    auto [left, right] = a.iter().partition<sus::Vec<i32>>(
        [](const i32& i) { return i % 2 == 1; });
    EXPECT_EQ(left, sus::Slice<i32>::from({1, 3, 5}));
    EXPECT_EQ(right, sus::Slice<i32>::from({2, 4}));
  }

  struct S {
    S(i32 i) : i(i) {}
    S(S&&) = default;
    S& operator=(S&&) = default;
    i32 i;
    bool operator==(const S& rhs) const noexcept { return i == rhs.i; }
  };
  static_assert(sus::mem::Move<S>);
  static_assert(!sus::mem::Copy<S>);
  static_assert(sus::cmp::Eq<S>);

  // Partition from into_iter() with Move and !Copy type.
  {
    auto it = sus::Array<S, 5>(S(1), S(2), S(3), S(4), S(5)).into_iter();
    static_assert(std::same_as<decltype(sus::move(it).partition<sus::Vec<S>>(
                                   [](const S&) { return true; })),
                               sus::Tuple<sus::Vec<S>, sus::Vec<S>>>);

    auto [left, right] = sus::move(it).partition<sus::Vec<S>>(
        [](const S& i) { return i.i % 2 == 1; });
    EXPECT_EQ(left, sus::Slice<S>::from({S(1), S(3), S(5)}));
    EXPECT_EQ(right, sus::Slice<S>::from({S(2), S(4)}));
  }

  // Test with simple Extend type.
  {
    auto [left, right] =
        sus::Array<i32, 5>(1, 2, 3, 4, 5)
            .into_iter()
            .partition<Extendable>([](const i32& i) { return i <= 3; });
    EXPECT_EQ(left.extend_sum, 1 + 2 + 3);
    EXPECT_EQ(right.extend_sum, 4 + 5);
  }

  static_assert(sus::Array<i32, 5>(1, 2, 3, 4, 5)
                    .into_iter()
                    .partition<Extendable>([](const i32& i) { return i <= 3; })
                    .into_inner<0>()
                    .extend_sum == 1 + 2 + 3);
  static_assert(sus::Array<i32, 5>(1, 2, 3, 4, 5)
                    .into_iter()
                    .partition<Extendable>([](const i32& i) { return i <= 3; })
                    .into_inner<1>()
                    .extend_sum == 4 + 5);
}

TEST(Iterator, Peekable) {
  // iter().
  {
    auto a = sus::Array<i32, 3>(1, 2, 3);
    auto it = a.iter().peekable();
    static_assert(sus::mem::Clone<decltype(it)>);
    static_assert(sus::mem::TriviallyRelocatable<decltype(it)>);
    static_assert(sus::iter::Iterator<decltype(it), const i32&>);
    static_assert(sus::iter::DoubleEndedIterator<decltype(it), const i32&>);
    static_assert(sus::iter::ExactSizeIterator<decltype(it), const i32&>);

    static_assert(std::same_as<decltype(it.peek()), sus::Option<const i32&>>);
    // The iterator is over const refs, so peek_mut has to return const.
    static_assert(
        std::same_as<decltype(it.peek_mut()), sus::Option<const i32&>>);

    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);

    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);

    EXPECT_EQ(it.peek().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);

    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);

    EXPECT_EQ(it.peek().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);

    EXPECT_EQ(it.peek().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);

    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);

    {
      // If next() sees the end first.
      auto it2 = sus::clone(it);
      EXPECT_EQ(it2.next(), sus::None);
      EXPECT_EQ(it2.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
      EXPECT_EQ(it2.exact_size_hint(), 0u);

      EXPECT_EQ(it2.peek(), sus::None);
      EXPECT_EQ(it2.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
      EXPECT_EQ(it2.exact_size_hint(), 0u);

      EXPECT_EQ(it2.next(), sus::None);
    }
    // If peek() sees the end first.
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);

    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next_back(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next(), sus::None);
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 3>(1, 2, 3);
    auto it = a.iter_mut().peekable();

    static_assert(std::same_as<decltype(it.peek()), sus::Option<const i32&>>);
    static_assert(std::same_as<decltype(it.peek_mut()), sus::Option<i32&>>);

    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.peek().unwrap(), 2);
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.peek().unwrap(), 3);
    EXPECT_EQ(it.peek().unwrap(), 3);
    EXPECT_EQ(it.next().unwrap(), 3);
    {
      // If next() sees the end first.
      auto it2 = sus::clone(it);
      EXPECT_EQ(it2.next(), sus::None);
      EXPECT_EQ(it2.peek(), sus::None);
      EXPECT_EQ(it2.next(), sus::None);
    }
    // If peek() sees the end first.
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next_back(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next(), sus::None);
  }
  // into_iter().
  {
    auto it = sus::Array<i32, 3>(1, 2, 3).into_iter().peekable();

    static_assert(std::same_as<decltype(it.peek()), sus::Option<const i32&>>);
    static_assert(std::same_as<decltype(it.peek_mut()), sus::Option<i32&>>);

    EXPECT_EQ(it.next().unwrap(), 1);
    EXPECT_EQ(it.peek().unwrap(), 2);
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.peek().unwrap(), 3);
    EXPECT_EQ(it.peek().unwrap(), 3);
    EXPECT_EQ(it.next().unwrap(), 3);
    {
      // If next() sees the end first.
      auto it2 = sus::clone(it);
      EXPECT_EQ(it2.next(), sus::None);
      EXPECT_EQ(it2.peek(), sus::None);
      EXPECT_EQ(it2.next(), sus::None);
    }
    // If peek() sees the end first.
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next_back(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.next(), sus::None);
  }

  // Interaction with next_back(), without peek().
  {
    auto it = sus::Array<i32, 3>(1, 2, 3).into_iter().peekable();
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next_back().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next_back().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next_back(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
  }

  // Interaction with next_back(), it does not change the peeked value until it
  // uses the peeked value last.
  {
    auto it = sus::Array<i32, 3>(1, 2, 3).into_iter().peekable();
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);

    EXPECT_EQ(it.peek().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);

    EXPECT_EQ(it.next_back().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);

    EXPECT_EQ(it.peek().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);

    EXPECT_EQ(it.next_back().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);

    EXPECT_EQ(it.peek().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);

    EXPECT_EQ(it.next_back().unwrap(), 1);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);

    EXPECT_EQ(it.peek(), sus::None);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);

    EXPECT_EQ(it.next(), sus::None);
    EXPECT_EQ(it.next_back(), sus::None);
    EXPECT_EQ(it.peek(), sus::None);
  }

  // next_if().
  {
    auto it = sus::Array<i32, 3>(1, 2, 3).into_iter().peekable();
    EXPECT_EQ(sus::none(), it.next_if([](const i32& i) {
      EXPECT_EQ(i, 1);
      return false;
    }));
    EXPECT_EQ(sus::none(), it.next_if([](const i32& i) {
      EXPECT_EQ(i, 1);
      return false;
    }));
    EXPECT_EQ(sus::some(1), it.next_if([](const i32& i) {
      EXPECT_EQ(i, 1);
      return true;
    }));
    EXPECT_EQ(sus::some(2), it.next_if([](const i32& i) {
      EXPECT_EQ(i, 2);
      return true;
    }));
    EXPECT_EQ(sus::none(), it.next_if([](const i32& i) {
      EXPECT_EQ(i, 3);
      return false;
    }));
    EXPECT_EQ(sus::some(3), it.next_if([](const i32& i) {
      EXPECT_EQ(i, 3);
      return true;
    }));
    EXPECT_EQ(sus::none(), it.next_if([](const i32&) {
      ADD_FAILURE();
      return true;
    }));
  }

  // next_if_eq().
  {
    auto it = sus::Array<i32, 3>(1, 2, 3).into_iter().peekable();
    EXPECT_EQ(sus::none(), it.next_if_eq(0));
    EXPECT_EQ(sus::none(), it.next_if_eq(0));
    EXPECT_EQ(sus::some(1), it.next_if_eq(1));
    EXPECT_EQ(sus::some(2), it.next_if_eq(2));
    EXPECT_EQ(sus::none(), it.next_if_eq(2));
    EXPECT_EQ(sus::some(3), it.next_if_eq(3));
    EXPECT_EQ(sus::none(), it.next_if_eq(3));
  }

  static_assert([]() {
    auto it = sus::Array<i32, 3>(1, 2, 3).into_iter().peekable();
    auto peeked = it.peek().unwrap();
    return peeked + it.next().unwrap();
  }() == 1 + 1);
}

TEST(Iterator, Position) {
  // iter().
  {
    auto a = sus::Array<i32, 8>(10, 11, 12, 13, 10, 11, 12, 13);
    auto it = a.iter();
    EXPECT_EQ(it.position([](const i32& i) { return i == 11; }), sus::some(1u));
    EXPECT_EQ(it.position([](const i32& i) { return i == 12; }), sus::some(0u));
    EXPECT_EQ(it.position([](const i32&) { return false; }), sus::none());
    EXPECT_EQ(it.position([](const i32&) { return true; }), sus::none());
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 8>(10, 11, 12, 13, 10, 11, 12, 13);
    auto it = a.iter_mut();
    EXPECT_EQ(it.position([](i32& i) { return i == 11; }), sus::some(1u));
    EXPECT_EQ(it.position([](i32& i) { return i == 12; }), sus::some(0u));
    EXPECT_EQ(it.position([](i32&) { return false; }), sus::none());
    EXPECT_EQ(it.position([](i32&) { return true; }), sus::none());
  }
  // into_iter().
  {
    auto it = sus::Array<i32, 8>(10, 11, 12, 13, 10, 11, 12, 13).into_iter();
    EXPECT_EQ(it.position([](i32&& i) { return i == 11; }), sus::some(1u));
    EXPECT_EQ(it.position([](i32&& i) { return i == 12; }), sus::some(0u));
    EXPECT_EQ(it.position([](i32&&) { return false; }), sus::none());
    EXPECT_EQ(it.position([](i32&&) { return true; }), sus::none());
  }

  // No match.
  {
    auto it = sus::Array<i32, 4>(10, 11, 12, 13).into_iter();
    EXPECT_EQ(it.position([](auto) { return false; }), sus::None);
  }

  static_assert(
      sus::Array<i32, 4>(10, 11, 12, 11).into_iter().position([](auto i) {
        return i == 11;
      }) == sus::some(1u));
  static_assert(
      sus::Array<i32, 4>(10, 11, 12, 11).into_iter().position([](auto i) {
        return i == 14;
      }) == sus::none());
}

TEST(Iterator, Product) {
  static_assert(::sus::iter::Product<i8>);
  static_assert(::sus::iter::Product<i16>);
  static_assert(::sus::iter::Product<i32>);
  static_assert(::sus::iter::Product<i64>);
  static_assert(::sus::iter::Product<isize>);
  static_assert(::sus::iter::Product<u8>);
  static_assert(::sus::iter::Product<u16>);
  static_assert(::sus::iter::Product<u32>);
  static_assert(::sus::iter::Product<u64>);
  static_assert(::sus::iter::Product<usize>);
  static_assert(::sus::iter::Product<f32>);
  static_assert(::sus::iter::Product<f64>);

  // Signed integer.
  {
    EXPECT_EQ((sus::Array<i32, 0>().into_iter().product()), 1);

    auto a = sus::Array<i32, 3>(2, 3, 4);
    decltype(auto) p = sus::move(a).into_iter().product();
    static_assert(std::same_as<decltype(p), i32>);
    EXPECT_EQ(p, 2 * 3 * 4);
  }
  // Unsigned integer.
  {
    EXPECT_EQ((sus::Array<u32, 0>().into_iter().product()), 1u);

    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    decltype(auto) p = sus::move(a).into_iter().product();
    static_assert(std::same_as<decltype(p), u32>);
    EXPECT_EQ(p, 2u * 3u * 4u);
  }
  // Float.
  {
    EXPECT_EQ((sus::Array<f32, 0>().into_iter().product()), 1.f);

    auto a = sus::Array<f32, 3>(2.f, 3.f, 4.f);
    decltype(auto) p = sus::move(a).into_iter().product();
    static_assert(std::same_as<decltype(p), f32>);
    EXPECT_EQ(p, 2.f * 3.f * 4.f);
  }

  static_assert(sus::Array<i32, 3>(2, 3, 4).into_iter().product() == 2 * 3 * 4);
  static_assert(sus::Array<u32, 3>(2u, 3u, 4u).into_iter().product() ==
                2u * 3u * 4u);
  static_assert(sus::Array<f32, 3>(2.f, 3.f, 4.f).into_iter().product() ==
                2.f * 3.f * 4.f);
}

TEST(Iterator, Reduce) {
  // Empty.
  {
    auto out =
        sus::Array<i32, 0>().into_iter().reduce([](i32, i32) { return 0; });
    EXPECT_EQ(out, sus::None);
  }
  // into_iter().
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto out =
        sus::move(a).into_iter().reduce([](i32 acc, i32 v) { return acc + v; });
    static_assert(std::same_as<decltype(out), sus::Option<i32>>);
    EXPECT_EQ(out, sus::some(2 + 3 + 4));
  }
  // iter().
  {
    auto a = sus::Array<i32, 3>(3, 2, 4);
    auto out = a.iter().reduce([](const i32& acc, const i32& v) -> const i32& {
      if (acc < v)
        return acc;
      else
        return v;
    });
    static_assert(std::same_as<decltype(out), sus::Option<const i32&>>);
    EXPECT_EQ(&out.as_value(), &a[1u]);
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 3>(3, 2, 4);
    auto out = a.iter_mut().reduce([](i32& acc, i32& v) -> i32& {
      if (acc < v)
        return acc;
      else
        return v;
    });
    static_assert(std::same_as<decltype(out), sus::Option<i32&>>);
    EXPECT_EQ(&out.as_value(), &a[1u]);
  }

  static_assert(sus::Array<i32, 3>(2, 3, 4)
                    .into_iter()
                    .reduce([](i32 acc, i32 v) { return acc + v; })
                    .unwrap() == 2 + 3 + 4);
}

TEST(Iterator, Reduce_Example_References) {
  auto a = sus::Array<i32, 3>(2, 3, 4);
  auto out = a.iter().copied().reduce([](i32 acc, i32 v) { return acc + v; });
  sus_check(out.as_value() == 2 + 3 + 4);
}

TEST(Iterator, Rposition) {
  // iter().
  {
    auto a = sus::Array<i32, 8>(10, 11, 12, 13, 10, 11, 12, 13);
    auto it = a.iter();
    EXPECT_EQ(it.rposition([](const i32& i) { return i == 11; }),
              sus::some(5u));
    EXPECT_EQ(it.rposition([](const i32& i) { return i == 12; }),
              sus::some(2u));
    EXPECT_EQ(it.rposition([](const i32&) { return false; }), sus::none());
    EXPECT_EQ(it.rposition([](const i32&) { return true; }), sus::none());
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 8>(10, 11, 12, 13, 10, 11, 12, 13);
    auto it = a.iter_mut();
    EXPECT_EQ(it.rposition([](i32& i) { return i == 11; }), sus::some(5u));
    EXPECT_EQ(it.rposition([](i32& i) { return i == 12; }), sus::some(2u));
    EXPECT_EQ(it.rposition([](i32&) { return false; }), sus::none());
    EXPECT_EQ(it.rposition([](i32&) { return true; }), sus::none());
  }
  // into_iter().
  {
    auto it = sus::Array<i32, 8>(10, 11, 12, 13, 10, 11, 12, 13).into_iter();
    EXPECT_EQ(it.rposition([](i32&& i) { return i == 11; }), sus::some(5u));
    EXPECT_EQ(it.rposition([](i32&& i) { return i == 12; }), sus::some(2u));
    EXPECT_EQ(it.rposition([](i32&&) { return false; }), sus::none());
    EXPECT_EQ(it.rposition([](i32&&) { return true; }), sus::none());
  }

  // No match.
  {
    auto it = sus::Array<i32, 4>(10, 11, 12, 13).into_iter();
    EXPECT_EQ(it.rposition([](auto) { return false; }), sus::None);
  }

  static_assert(
      sus::Array<i32, 4>(10, 11, 12, 11).into_iter().rposition([](auto i) {
        return i == 11;
      }) == sus::some(3u));
}

TEST(Iterator, Scan) {
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto it = sus::move(a).into_iter().scan(
        22_i32, [](i32& state, i32&& v) -> sus::Option<std::string> {
          auto before = state;
          if (v % 2 == 0) state += v;
          return sus::some(fmt::format("{} + {} = {}", before, v, state));
        });
    static_assert(sus::mem::Clone<decltype(it)>);
    EXPECT_EQ(it.next(), sus::some("22 + 2 = 24"));
    EXPECT_EQ(it.next(), sus::some("24 + 3 = 24"));
    EXPECT_EQ(it.next(), sus::some("24 + 4 = 28"));
    EXPECT_EQ(it.next(), sus::none());
  }
  // Early terminate.
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto it = sus::move(a).into_iter().scan(
        22_i32, [](i32& state, i32&& v) -> sus::Option<std::string> {
          auto before = state;
          if (v < 4) {
            state += v;
            return sus::some(fmt::format("{} + {} = {}", before, v, state));
          } else {
            return sus::none();
          }
        });
    static_assert(sus::mem::Clone<decltype(it)>);
    EXPECT_EQ(it.next(), sus::some("22 + 2 = 24"));
    EXPECT_EQ(it.next(), sus::some("24 + 3 = 27"));
    EXPECT_EQ(it.next(), sus::none());
  }

  static_assert(sus::Array<i32, 3>(2, 3, 4)
                    .into_iter()
                    .scan(10_i32,
                          [](i32& state, i32 v) -> sus::Option<i32> {
                            state += 1;
                            return sus::some(state * v);
                          })
                    .sum() == (11 * 2 + 12 * 3 + 13 * 4));
}

TEST(Iterator, Skip) {
  // Skip nothing.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().skip(0u);
    static_assert(sus::iter::Iterator<decltype(it), i32>);
    static_assert(sus::iter::DoubleEndedIterator<decltype(it), i32>);
    static_assert(sus::iter::ExactSizeIterator<decltype(it), i32>);
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));

    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));

    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));

    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));

    EXPECT_EQ(it.next(), sus::none());
  }
  // Skip everything.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().skip(10u);
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
  }
  // Skip some.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().skip(2u);
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.next(), sus::none());
  }

  // next_back() before skippping.
  {
    auto it = sus::Array<i32, 5>(2, 3, 4, 5, 6).into_iter().skip(3u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.next_back().unwrap(), 6);
    EXPECT_EQ(it.next_back().unwrap(), 5);
    EXPECT_EQ(it.next_back(), sus::none());
  }
  // next_back() then skippping.
  {
    auto it = sus::Array<i32, 5>(2, 3, 4, 5, 6).into_iter().skip(3u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.next_back().unwrap(), 6);
    EXPECT_EQ(it.next().unwrap(), 5);
    EXPECT_EQ(it.next_back(), sus::none());
  }
  // next_back() skips everything.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().skip(10u);
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.next_back(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next_back(), sus::none());
  }

  // iter().
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto it = a.iter().skip(1u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    static_assert(std::same_as<decltype(it.next()), sus::Option<const i32&>>);
    EXPECT_EQ(&it.next().unwrap(), &a[1u]);
    EXPECT_EQ(&it.next().unwrap(), &a[2u]);
    EXPECT_EQ(it.next(), sus::none());
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto it = a.iter_mut().skip(1u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    static_assert(std::same_as<decltype(it.next()), sus::Option<i32&>>);
    EXPECT_EQ(&it.next().unwrap(), &a[1u]);
    EXPECT_EQ(&it.next().unwrap(), &a[2u]);
    EXPECT_EQ(it.next(), sus::none());
  }

  static_assert(
      sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().skip(4u).sum() == 6 + 7);
}

TEST(Iterator, SkipWhile) {
  // Skip nothing.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().skip_while(
        [](const i32&) { return false; });
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(3u)));
    EXPECT_EQ(it.next().unwrap(), 2);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(2u)));
    EXPECT_EQ(it.next().unwrap(), 3);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(1u)));
    EXPECT_EQ(it.next().unwrap(), 4);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.next(), sus::none());
  }
  // Skip everything.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().skip_while(
        [](const i32&) { return true; });
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(3u)));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
  }
  // Skip some.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().skip_while(
        [](const i32& i) { return i <= 3; });
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(3u)));
    EXPECT_EQ(it.next(), sus::some(4));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
  }
  // Closure isn't called after false.
  {
    auto it = sus::Array<i32, 5>(2, 3, 4, 1, 0)
                  .into_iter()
                  .skip_while([](const i32& i) { return i <= 3; });
    EXPECT_EQ(it.next(), sus::some(4));
    EXPECT_EQ(it.next(), sus::some(1));
    EXPECT_EQ(it.next(), sus::some(0));
    EXPECT_EQ(it.next(), sus::none());
  }

  // iter().
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto it = a.iter().skip_while([](const i32& i) { return i <= 3; });
    static_assert(std::same_as<decltype(it.next()), sus::Option<const i32&>>);
    EXPECT_EQ(&it.next().unwrap(), &a[2u]);
    EXPECT_EQ(it.next(), sus::none());
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto it = a.iter_mut().skip_while([](const i32& i) { return i <= 3; });
    static_assert(std::same_as<decltype(it.next()), sus::Option<i32&>>);
    EXPECT_EQ(&it.next().unwrap(), &a[2u]);
    EXPECT_EQ(it.next(), sus::none());
  }

  static_assert(sus::Array<i32, 6>(2, 3, 4, 5, 6, 7)
                    .into_iter()
                    .skip_while([](i32 i) { return i <= 4; })
                    .sum() == 5 + 6 + 7);
}

TEST(Iterator, StepBy) {
  // Step by 0.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().step_by(1u);
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next(), sus::some(3));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next(), sus::some(4));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
  }
  // Step by more.
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(2u);
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next(), sus::some(4));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next(), sus::some(6));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(2u);
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next_back(), sus::some(6));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next_back(), sus::some(4));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next_back(), sus::some(2));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next_back(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(3u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next(), sus::some(5));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(3u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next_back(), sus::some(5));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next_back(), sus::some(2));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
  }
  // next() then next_back().
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(2u);
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next_back(), sus::some(6));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next_back(), sus::some(4));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next_back(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
  }
  // The two ends.
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(5u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.next(), sus::some(7));
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(5u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.next_back(), sus::some(7));
    EXPECT_EQ(it.next_back(), sus::none());
  }
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(5u);
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next_back(), sus::some(7));
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.next(), sus::none());
  }
  // Step by more than length.
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(8u);
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto it = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7).into_iter().step_by(8u);
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next_back(), sus::some(2));
    EXPECT_EQ(it.next_back(), sus::none());
  }

  static_assert(
      sus::Array<i32, 7>(2, 3, 4, 5, 6, 7, 8).into_iter().step_by(3u).sum() ==
      2 + 5 + 8);
}

TEST(Iterator, Sum) {
  static_assert(::sus::iter::Sum<i8>);
  static_assert(::sus::iter::Sum<i16>);
  static_assert(::sus::iter::Sum<i32>);
  static_assert(::sus::iter::Sum<i64>);
  static_assert(::sus::iter::Sum<isize>);
  static_assert(::sus::iter::Sum<u8>);
  static_assert(::sus::iter::Sum<u16>);
  static_assert(::sus::iter::Sum<u32>);
  static_assert(::sus::iter::Sum<u64>);
  static_assert(::sus::iter::Sum<usize>);
  static_assert(::sus::iter::Sum<f32>);
  static_assert(::sus::iter::Sum<f64>);

  // Signed integer.
  {
    EXPECT_EQ((sus::Array<i32, 0>().into_iter().sum()), 0);

    auto a = sus::Array<i32, 3>(2, 3, 4);
    decltype(auto) p = sus::move(a).into_iter().sum();
    static_assert(std::same_as<decltype(p), i32>);
    EXPECT_EQ(p, 2 + 3 + 4);
  }
  // Unsigned integer.
  {
    EXPECT_EQ((sus::Array<u32, 0>().into_iter().sum()), 0u);

    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    decltype(auto) p = sus::move(a).into_iter().sum();
    static_assert(std::same_as<decltype(p), u32>);
    EXPECT_EQ(p, 2u + 3u + 4u);
  }
  // Float.
  {
    EXPECT_EQ((sus::Array<f32, 0>().into_iter().sum()), 0.f);

    auto a = sus::Array<f32, 3>(2.f, 3.f, 4.f);
    decltype(auto) p = sus::move(a).into_iter().sum();
    static_assert(std::same_as<decltype(p), f32>);
    EXPECT_EQ(p, 2.f + 3.f + 4.f);
  }

  static_assert(sus::Array<i32, 3>(2, 3, 4).into_iter().sum() == 2 + 3 + 4);
  static_assert(sus::Array<f32, 3>(2.f, 3.f, 4.f).into_iter().sum() ==
                2.f + 3.f + 4.f);
}

TEST(Iterator, Take) {
  // Take none.
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = sus::move(a).into_iter().take(0u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = sus::move(a).into_iter().take(0u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next_back(), sus::none());
    EXPECT_EQ(it.next_back(), sus::none());
  }
  // Take all.
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = sus::move(a).into_iter().take(3u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next(), sus::some(2u));
    EXPECT_EQ(it.next(), sus::some(3u));
    EXPECT_EQ(it.next(), sus::some(4u));
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = sus::move(a).into_iter().take(3u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next_back(), sus::some(4u));
    EXPECT_EQ(it.next_back(), sus::some(3u));
    EXPECT_EQ(it.next_back(), sus::some(2u));
    EXPECT_EQ(it.next_back(), sus::none());
  }
  // Take more than all.
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = sus::move(a).into_iter().take(5u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next(), sus::some(2u));
    EXPECT_EQ(it.next(), sus::some(3u));
    EXPECT_EQ(it.next(), sus::some(4u));
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = sus::move(a).into_iter().take(5u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next_back(), sus::some(4u));
    EXPECT_EQ(it.next_back(), sus::some(3u));
    EXPECT_EQ(it.next_back(), sus::some(2u));
    EXPECT_EQ(it.next(), sus::none());
  }
  // Take some.
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = sus::move(a).into_iter().take(2u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next(), sus::some(2u));
    EXPECT_EQ(it.next(), sus::some(3u));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
  }
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = sus::move(a).into_iter().take(2u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next_back(), sus::some(3u));
    EXPECT_EQ(it.next_back(), sus::some(2u));
    EXPECT_EQ(it.next_back(), sus::none());
    EXPECT_EQ(it.next_back(), sus::none());
  }
  // Mix of front and back.
  {
    auto a = sus::Array<u32, 5>(2u, 3u, 4u, 5u, 6u);
    auto it = sus::move(a).into_iter().take(3u);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next(), sus::some(2u));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next_back(), sus::some(4u));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next(), sus::some(3u));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
  }
  // iter().
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = a.iter().take(2u);
    static_assert(std::same_as<decltype(it.next()), sus::Option<const u32&>>);
    EXPECT_EQ(&it.next().unwrap(), &a[0u]);
    EXPECT_EQ(&it.next().unwrap(), &a[1u]);
    EXPECT_EQ(it.next(), sus::none());
  }
  // iter_mut().
  {
    auto a = sus::Array<u32, 3>(2u, 3u, 4u);
    auto it = a.iter_mut().take(2u);
    static_assert(std::same_as<decltype(it.next()), sus::Option<u32&>>);
    EXPECT_EQ(&it.next().unwrap(), &a[0u]);
    EXPECT_EQ(&it.next().unwrap(), &a[1u]);
    EXPECT_EQ(it.next(), sus::none());
  }

  static_assert(sus::Array<i32, 5>(2, 3, 4, 5, 6).into_iter().take(3u).sum() ==
                2 + 3 + 4);
}

TEST(Iterator, TakeWhile) {
  // Take nothing.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().take_while(
        [](const i32&) { return false; });
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(3u)));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
  }
  // Take everything.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().take_while(
        [](const i32&) { return true; });
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(3u)));
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(2u)));
    EXPECT_EQ(it.next(), sus::some(3));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(1u)));
    EXPECT_EQ(it.next(), sus::some(4));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
  }
  // Take some.
  {
    auto it = sus::Array<i32, 3>(2, 3, 4).into_iter().take_while(
        [](const i32& i) { return i <= 3; });
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(3u)));
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(2u)));
    EXPECT_EQ(it.next(), sus::some(3));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(1u)));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
  }
  // Closure isn't called after false.
  {
    auto it = sus::Array<i32, 5>(2, 3, 4, 1, 0)
                  .into_iter()
                  .take_while([](const i32& i) { return i <= 3; });
    EXPECT_EQ(it.next(), sus::some(2));
    EXPECT_EQ(it.next(), sus::some(3));
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
    EXPECT_EQ(it.next(), sus::none());
  }

  // iter().
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto it = a.iter().take_while([](const i32& i) { return i <= 3; });
    static_assert(std::same_as<decltype(it.next()), sus::Option<const i32&>>);
    EXPECT_EQ(&it.next().unwrap(), &a[0u]);
    EXPECT_EQ(&it.next().unwrap(), &a[1u]);
    EXPECT_EQ(it.next(), sus::none());
  }
  // iter_mut().
  {
    auto a = sus::Array<i32, 3>(2, 3, 4);
    auto it = a.iter_mut().take_while([](const i32& i) { return i <= 3; });
    static_assert(std::same_as<decltype(it.next()), sus::Option<i32&>>);
    EXPECT_EQ(&it.next().unwrap(), &a[0u]);
    EXPECT_EQ(&it.next().unwrap(), &a[1u]);
    EXPECT_EQ(it.next(), sus::none());
  }

  static_assert(sus::Array<i32, 3>(2, 3, 4)
                    .into_iter()
                    .take_while([](i32 i) constexpr { return i <= 3; })
                    .sum() == 2 + 3);
}

TEST(Iterator, TryFold) {
  // With Option.
  {
    auto a = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7);
    auto it = sus::move(a).into_iter();
    auto f = [](std::string acc, i32 i) -> Option<std::string> {
      // Stop at 4.
      if (i % 4 == 0) return sus::none();
      // Record odd numbers.
      if (i % 2 == 1) return sus::some(fmt::format("{}{}", acc, i));
      // Do nothing with even numbers.
      return sus::some(acc);
    };
    auto o = it.try_fold(std::string(), f);
    static_assert(std::same_as<decltype(o), Option<std::string>>);
    EXPECT_EQ(o, sus::none());  // Stopped at 4.
    o = it.try_fold(std::string(), f);
    EXPECT_EQ(o, sus::some("57"));  // Got to the end.
  }
  // With Result.
  {
    auto a = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7);
    auto it = sus::move(a).into_iter();
    auto f = [](std::string acc,
                i32 i) -> sus::result::Result<std::string, i32> {
      // Stop at 4.
      if (i % 4 == 0) return sus::err(i);
      // Record odd numbers.
      if (i % 2 == 1) return sus::ok(fmt::format("{}{}", acc, i));
      // Do nothing with even numbers.
      return sus::ok(acc);
    };
    auto o = it.try_fold(std::string(), f);
    static_assert(
        std::same_as<decltype(o), sus::result::Result<std::string, i32>>);
    EXPECT_EQ(o, sus::err(4));  // Stopped at 4.
    o = it.try_fold(std::string(), f);
    EXPECT_EQ(o, sus::ok("57"));  // Got to the end.
  }

  static_assert(sus::Array<i32, 6>(2, 3, 4, 5, 6, 7)
                    .into_iter()
                    .try_fold(0_i32, [](i32 acc, i32 i) -> Option<i32> {
                      if (i < 3) return sus::some(acc);
                      if (i < 6) return sus::some(acc + i);
                      return sus::some(acc * 2);
                    }) == sus::some((3 + 4 + 5) * 2 * 2));
  static_assert(sus::Array<i32, 6>(2, 3, 4, 5, 6, 7)
                    .into_iter()
                    .try_fold(0_i32, [](i32 acc, i32 i) -> Option<i32> {
                      if (i < 3) return sus::some(acc);
                      return sus::none();
                    }) == sus::none());
}

TEST(Iterator, TryRfold) {
  // With Option.
  {
    auto a = sus::Array<i32, 6>(1, 2, 3, 4, 5, 6);
    auto it = sus::move(a).into_iter();
    auto f = [](std::string acc, i32 i) -> Option<std::string> {
      // Stop at 4.
      if (i % 4 == 0) return sus::none();
      // Record odd numbers.
      if (i % 2 == 1) return sus::some(fmt::format("{}{}", acc, i));
      // Do nothing with even numbers.
      return sus::some(acc);
    };
    auto o = it.try_rfold(std::string(), f);
    static_assert(std::same_as<decltype(o), Option<std::string>>);
    EXPECT_EQ(o, sus::none());  // Stopped at 4.
    o = it.try_rfold(std::string(), f);
    EXPECT_EQ(o, sus::some("31"));  // Got to the end.
  }
  // With Result.
  {
    auto a = sus::Array<i32, 6>(1, 2, 3, 4, 5, 6);
    auto it = sus::move(a).into_iter();
    auto f = [](std::string acc,
                i32 i) -> sus::result::Result<std::string, i32> {
      // Stop at 4.
      if (i % 4 == 0) return sus::err(i);
      // Record odd numbers.
      if (i % 2 == 1) return sus::ok(fmt::format("{}{}", acc, i));
      // Do nothing with even numbers.
      return sus::ok(acc);
    };
    auto o = it.try_rfold(std::string(), f);
    static_assert(
        std::same_as<decltype(o), sus::result::Result<std::string, i32>>);
    EXPECT_EQ(o, sus::err(4));  // Stopped at 4.
    o = it.try_rfold(std::string(), f);
    EXPECT_EQ(o, sus::ok("31"));  // Got to the end.
  }

  static_assert(sus::Array<i32, 6>(2, 3, 4, 5, 6, 7)
                    .into_iter()
                    .try_rfold(0_i32, [](i32 acc, i32 i) -> Option<i32> {
                      if (i < 3) return sus::some(acc);
                      if (i < 6) return sus::some(acc + i);
                      return sus::some(acc * 2);
                    }) == sus::some((0 * 2 * 2) + 3 + 4 + 5));
  static_assert(sus::Array<i32, 6>(2, 3, 4, 5, 6, 7)
                    .into_iter()
                    .try_rfold(0_i32, [](i32 acc, i32 i) -> Option<i32> {
                      if (i < 3) return sus::some(acc);
                      return sus::none();
                    }) == sus::none());
}

TEST(Iterator, TryForEach) {
  struct Void {};

  // With Option.
  {
    auto a = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7);
    auto it = sus::move(a).into_iter();
    std::string visited;
    auto f = [&visited](i32 i) -> Option<Void> {
      // Stop at 4.
      if (i % 4 == 0) return sus::none();
      // Record other numbers.
      visited = fmt::format("{}{}", visited, i);
      return sus::some(Void());
    };
    auto o = it.try_for_each(f);
    static_assert(std::same_as<decltype(o), Option<Void>>);
    EXPECT_EQ(o.is_some(), false);  // Stopped at 4.
    EXPECT_EQ(visited, "23");
    o = it.try_for_each(f);
    EXPECT_EQ(o.is_some(), true);  // Got to the end.
    EXPECT_EQ(visited, "23567");
  }
  // With Result.
  {
    auto a = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7);
    auto it = sus::move(a).into_iter();
    std::string visited;
    auto f = [&visited](i32 i) -> sus::result::Result<Void, i32> {
      // Stop at 4.
      if (i % 4 == 0) return sus::err(i);
      // Record other numbers.
      visited = fmt::format("{}{}", visited, i);
      return sus::ok(Void());
    };
    auto o = it.try_for_each(f);
    static_assert(std::same_as<decltype(o), sus::result::Result<Void, i32>>);
    EXPECT_EQ(o.as_err(), 4);  // Stopped at 4.
    EXPECT_EQ(visited, "23");
    o = it.try_for_each(f);
    EXPECT_EQ(o.is_ok(), true);  // Got to the end.
    EXPECT_EQ(visited, "23567");
  }
  // With void Result.
  {
    auto a = sus::Array<i32, 6>(2, 3, 4, 5, 6, 7);
    auto it = sus::move(a).into_iter();
    std::string visited;
    auto f = [&visited](i32 i) -> sus::result::Result<void, i32> {
      // Stop at 4.
      if (i % 4 == 0) return sus::err(i);
      // Record other numbers.
      visited = fmt::format("{}{}", visited, i);
      return sus::ok();
    };
    auto o = it.try_for_each(f);
    static_assert(std::same_as<decltype(o), sus::result::Result<void, i32>>);
    EXPECT_EQ(o.as_err(), 4);  // Stopped at 4.
    EXPECT_EQ(visited, "23");
    o = it.try_for_each(f);
    EXPECT_EQ(o.is_ok(), true);  // Got to the end.
    EXPECT_EQ(visited, "23567");
  }

  static_assert([] {
    i32 sum;
    auto r = sus::Array<i32, 3>(2, 3, 4).into_iter().try_for_each(
        [&](i32 i) -> sus::result::Result<void, i32> {
          sum += i;
          return sus::ok();
        });
    sus_check(r.is_ok());
    return sum;
  }() == 2 + 3 + 4);
}

TEST(Iterator, Unzip) {
  using sus::Tuple;
  using sus::Vec;
  {
    auto a = sus::Array<Tuple<i32, f32>, 0>();
    auto u = sus::move(a).into_iter().unzip<Vec<i32>, Vec<f32>>();
    static_assert(std::same_as<decltype(u), Tuple<Vec<i32>, Vec<f32>>>);
    EXPECT_EQ(u.at<0>().is_empty(), true);
    EXPECT_EQ(u.at<1>().is_empty(), true);
  }
  {
    auto a = sus::Array<Tuple<i32, f32>, 5>(  //
        ::sus::tuple(1, 2.f),                 //
        ::sus::tuple(2, 3.f),                 //
        ::sus::tuple(3, 4.f),                 //
        ::sus::tuple(4, 5.f),                 //
        ::sus::tuple(5, 6.f));
    auto u = sus::move(a).into_iter().unzip<Vec<i32>, Vec<f32>>();
    static_assert(std::same_as<decltype(u), Tuple<Vec<i32>, Vec<f32>>>);
    EXPECT_EQ(u.at<0>(), sus::Slice<i32>::from({1, 2, 3, 4, 5}));
    EXPECT_EQ(u.at<1>(), sus::Slice<f32>::from({2.f, 3.f, 4.f, 5.f, 6.f}));
  }

  static_assert(sus::Array<Tuple<i32, f32>, 5>(::sus::tuple(1, 2.f),  //
                                               ::sus::tuple(2, 3.f),  //
                                               ::sus::tuple(3, 4.f),  //
                                               ::sus::tuple(4, 5.f),  //
                                               ::sus::tuple(5, 6.f))
                    .into_iter()
                    .unzip<Vec<i32>, Vec<f32>>()
                    .into_inner<0>()
                    .into_iter()
                    .sum() == 1 + 2 + 3 + 4 + 5);
}

TEST(Iterator, Zip) {
  // Equal sizes, both sides are values.
  {
    auto a = sus::Array<i32, 5>(2, 3, 4, 5, 6);
    auto b = sus::Array<f32, 5>(3.f, 4.f, 5.f, 6.f, 7.f);
    auto it = sus::move(a).into_iter().zip(sus::move(b));
    static_assert(
        std::same_as<decltype(it.next()), sus::Option<sus::Tuple<i32, f32>>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(5u, sus::some(5u)));
    EXPECT_EQ(it.exact_size_hint(), 5u);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(2, 3.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(4u, sus::some(4u)));
    EXPECT_EQ(it.exact_size_hint(), 4u);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(3, 4.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(3u, sus::some(3u)));
    EXPECT_EQ(it.exact_size_hint(), 3u);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(4, 5.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(5, 6.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(6, 7.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
  }
  // First ends first, both sides are values.
  {
    auto a = sus::Array<i32, 2>(2, 3);
    auto b = sus::Array<f32, 5>(3.f, 4.f, 5.f, 6.f, 7.f);
    auto it = sus::move(a).into_iter().zip(sus::move(b));
    static_assert(
        std::same_as<decltype(it.next()), sus::Option<sus::Tuple<i32, f32>>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.exact_size_hint(), 2u);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(2, 3.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(3, 4.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());  // Eats 5.f.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());  // Eats 6.f.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());  // Eats 7.f.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
  }
  // First ends second, both sides are values.
  {
    auto a = sus::Array<i32, 5>(2, 3, 4, 5, 6);
    auto b = sus::Array<f32, 2>(3.f, 4.f);
    auto it = sus::move(a).into_iter().zip(sus::move(b));
    static_assert(
        std::same_as<decltype(it.next()), sus::Option<sus::Tuple<i32, f32>>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(2u, sus::some(2u)));
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(2, 3.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(1u, sus::some(1u)));
    EXPECT_EQ(it.exact_size_hint(), 1u);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(3, 4.f)));
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());  // Eats 4.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());  // Eats 5.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());  // Eats 6.
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
  }
  // Empty.
  {
    auto a = sus::Array<i32, 0>();
    auto b = sus::Array<f32, 0>();
    auto it = sus::move(a).into_iter().zip(sus::move(b));
    static_assert(
        std::same_as<decltype(it.next()), sus::Option<sus::Tuple<i32, f32>>>);
    EXPECT_EQ(it.size_hint(), sus::iter::SizeHint(0u, sus::some(0u)));
    EXPECT_EQ(it.exact_size_hint(), 0u);
    EXPECT_EQ(it.next(), sus::none());
  }
  // zip().
  {
    auto a = sus::Array<i32, 2>(2, 3);
    auto b = sus::Array<f32, 5>(3.f, 4.f, 5.f, 6.f, 7.f);
    auto it = sus::iter::zip(::sus::move(a), ::sus::move(b));
    static_assert(
        std::same_as<decltype(it.next()), sus::Option<sus::Tuple<i32, f32>>>);
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(2, 3.f)));
    EXPECT_EQ(it.next().unwrap(), (sus::Tuple<i32, f32>(3, 4.f)));
    EXPECT_EQ(it.next(), sus::none());
  }

  static_assert(
      sus::iter::zip(sus::Array<i32, 2>(2, 3),
                     sus::Array<f32, 5>(3.f, 4.f, 5.f, 6.f, 7.f))
          .map([](sus::Tuple<i32, f32> pair) { return pair.template at<0>(); })
          .sum() == 2 + 3);
}

TEST(Iterator, Zip_Example) {
  auto a = sus::Array<i32, 2>(2, 3);
  auto b = sus::Array<f32, 5>(3.f, 4.f, 5.f, 6.f, 7.f);
  auto it = sus::iter::zip(::sus::move(a), ::sus::move(b));
  sus_check(it.next() == sus::some(sus::tuple(2, 3.f)));
  sus_check(it.next() == sus::some(sus::tuple(3, 4.f)));
  sus_check(it.next() == sus::none());
}

TEST(Iterator, IsSorted) {
  EXPECT_EQ((sus::Array<i32, 0>().into_iter().is_sorted()), true);
  EXPECT_EQ(sus::Slice<i32>::from({5}).into_iter().is_sorted(), true);
  EXPECT_EQ(
      sus::Slice<i32>::from({1, 3, 4, 4, 4, 5, 5, 6}).into_iter().is_sorted(),
      true);
  EXPECT_EQ(sus::Slice<i32>::from({1, 3}).into_iter().is_sorted(), true);
  EXPECT_EQ(sus::Slice<i32>::from({3, 3}).into_iter().is_sorted(), true);
  EXPECT_EQ(sus::Slice<i32>::from({13, 3}).into_iter().is_sorted(), false);
  EXPECT_EQ(sus::Slice<i32>::from({1, 3, 4, 4, 4, 5, 5, 6, 4})
                .into_iter()
                .is_sorted(),
            false);
  EXPECT_EQ(sus::Slice<i32>::from({1, 3, 4, 4, 4, 2, 5, 5, 6})
                .into_iter()
                .is_sorted(),
            false);

  static_assert(sus::Slice<i32>::from({1, 2, 3, 4, 4, 4, 5, 5, 6})
                    .into_iter()
                    .is_sorted() == true);
  static_assert(sus::Slice<i32>::from({1, 3, 4, 4, 4, 2, 5, 5, 6})
                    .into_iter()
                    .is_sorted() == false);
}

TEST(Iterator, IsSortedBy) {
  auto cmp = [](f32 a, f32 b) {
    sus_check(!a.is_nan() && !b.is_nan());
    if (a < b) return std::strong_ordering::less;
    if (a > b) return std::strong_ordering::greater;
    return std::strong_ordering::equivalent;
  };

  EXPECT_EQ((sus::Array<f32, 0>().into_iter().is_sorted_by(cmp)), true);
  EXPECT_EQ(sus::Slice<f32>::from({5.f}).into_iter().is_sorted_by(cmp), true);
  EXPECT_EQ(sus::Slice<f32>::from({1.f, 3.f, 4.f, 4.f, 4.f, 5.f, 5.f, 6.f})
                .into_iter()
                .is_sorted_by(cmp),
            true);
  EXPECT_EQ(sus::Slice<f32>::from({1.f, 3.f}).into_iter().is_sorted_by(cmp),
            true);
  EXPECT_EQ(sus::Slice<f32>::from({3.f, 3.f}).into_iter().is_sorted_by(cmp),
            true);
  EXPECT_EQ(sus::Slice<f32>::from({13.f, 3.f}).into_iter().is_sorted_by(cmp),
            false);
  EXPECT_EQ(sus::Slice<f32>::from({1.f, 3.f, 4.f, 4.f, 4.f, 5.f, 5.f, 6.f, 4.f})
                .into_iter()
                .is_sorted_by(cmp),
            false);
  EXPECT_EQ(sus::Slice<f32>::from({1.f, 3.f, 4.f, 4.f, 4.f, 2.f, 5.f, 5.f, 6.f})
                .into_iter()
                .is_sorted_by(cmp),
            false);

  static_assert(
      sus::Slice<f32>::from({1.f, 2.f, 3.f}).into_iter().is_sorted_by(cmp) ==
      true);
  static_assert(
      sus::Slice<f32>::from({1.f, 3.f, 2.f}).into_iter().is_sorted_by(cmp) ==
      false);
}

}  // namespace
