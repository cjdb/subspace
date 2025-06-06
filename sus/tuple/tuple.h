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

#pragma once

#include <compare>
#include <concepts>
#include <utility>  // TODO: Replace std::index_sequence.

#include "fmt/core.h"
#include "sus/assertions/check.h"
#include "sus/choice/__private/pack_index.h"
#include "sus/cmp/eq.h"
#include "sus/cmp/ord.h"
#include "sus/construct/default.h"
#include "sus/construct/safe_from_reference.h"
#include "sus/iter/extend.h"
#include "sus/iter/into_iterator.h"
#include "sus/lib/__private/forward_decl.h"
#include "sus/macros/__private/compiler_bugs.h"
#include "sus/macros/lifetimebound.h"
#include "sus/macros/no_unique_address.h"
#include "sus/mem/clone.h"
#include "sus/mem/copy.h"
#include "sus/mem/forward.h"
#include "sus/mem/move.h"
#include "sus/mem/remove_rvalue_reference.h"
#include "sus/mem/replace.h"
#include "sus/num/num_concepts.h"
#include "sus/string/__private/any_formatter.h"
#include "sus/string/__private/format_to_stream.h"
#include "sus/tuple/__private/storage.h"

namespace sus {
/// The [`Tuple`]($sus::tuple_type::Tuple) type, and the
/// [`tuple`]($sus::tuple_type::tuple) type-deduction constructor function.
namespace tuple_type {}
}  // namespace sus

namespace sus::tuple_type {

/// A Tuple is a finite sequence of one or more heterogeneous values.
///
/// The Tuple is similar to
/// [std::tuple](https://en.cppreference.com/w/cpp/utility/tuple) with some
/// differences:
/// * It allows storing reference types.
/// * It interacts with iterators through satisfying
///   [`Extend`]($sus::iter::Extend), allowing an iterator to `unzip()` into
///   a Tuple of collections that satisfy Extend.
/// * It provides explicit methods for const, mutable or rvalue access to
///   its values.
/// * It satisfies [`Clone`]($sus::mem::Clone) if its elements all satisfy
///   `Clone`.
///
/// Tuple elements can also be accessed through `get()` for code that wants to
/// work generically over tuple-like objects including `sus::Tuple` and
/// `std::tuple`.
///
/// # Tail padding
/// The Tuple's tail padding may be reused when the Tuple is marked as
/// `[[no_unique_address]]`. The Tuple will have tail padding if the first
/// type has a size that is not a multiple of the Tuple's alignment. For
/// example if it's smaller than the alignment, such as `Tuple<u8, u64>` which
/// has `(alignof(u64) == sizeof(u64)) - sizeof(u8)` or 7 bytes of tail padding.
///
/// ```
/// struct S {
///   [[no_unique_address]] Tuple<u32, u64> tuple;  // 16 bytes.
///   u32 val;  // 4 bytes.
/// };  // 16 bytes, since `val` is stored inside `tuple`.
/// ```
///
/// However note that this behaviour is compiler-dependent, and MSVC does not
/// use the `[[no_unique_address]]` hint.
///
/// Use `sus::data_size_of<T>()` to determine the size of T excluding its tail
/// padding (so `sus::size_of<T>() - sus::data_size_of<T>()` is the tail
/// padding), which can be useful to ensure you have the expected behaviour from
/// your types.
///
/// Additionally types within the tuple may be placed inside the tail padding of
/// other types in the tuple, should such padding exist.
///
/// Generally, but not always, use of tail padding in Tuple is optimized by
/// ordering types (left-to-right in the template variables) from smallest-to-
/// largest for simple types such as integers (which have no tail padding
/// themselves), or in least-to-most tail-padding for more complex types.
/// Elements in a Tuple are stored internally in reverse of the order they are
/// specified, which is why the size of the *first* element matters for the
/// Tuple's externally usable tail padding.
template <class T, class... Ts>
class Tuple final {
 public:
  /// Construct a Tuple with the default value for the types it contains.
  ///
  /// The Tuple's contained types must all be #Default, and will be
  /// constructed through that trait.
  /// #[doc.overloads=ctor.default]
  // clang-format off
  explicit inline constexpr Tuple() noexcept
    requires(!((std::is_trivially_default_constructible_v<T> && ... &&
                std::is_trivially_default_constructible_v<Ts>) &&
              !(std::is_reference_v<T> || ... || std::is_reference_v<Ts>)
             ) &&
             (::sus::construct::Default<T> && ... &&
              ::sus::construct::Default<Ts>))
      : Tuple(T(), Ts()...) {}
  // clang-format on

  /// #[doc.overloads=ctor.default]
  explicit Tuple()
    requires((std::is_trivially_default_constructible_v<T> && ... &&
              std::is_trivially_default_constructible_v<Ts>) &&
             !(std::is_reference_v<T> || ... || std::is_reference_v<Ts>))
  = default;

  /// Construct a Tuple with the given values.
  ///
  /// # Const References
  ///
  /// For `Result<const T&, E>` it is possible to bind to a temporary which
  /// would create a memory safety bug. The `[[clang::lifetimebound]]` attribute
  /// is used to prevent this via Clang. But additionally, the incoming type is
  /// required to match with `sus::construct::SafelyConstructibleFromReference`
  /// to prevent conversions that would construct a temporary.
  ///
  /// To force accepting a const reference anyway in cases where a type can
  /// convert to a reference without constructing a temporary, use an unsafe
  /// `static_cast<const T&>()` at the callsite (and document it =)).
  /// #[doc.overloads=ctor.values]
  template <std::convertible_to<T> U, std::convertible_to<Ts>... Us>
    requires(sizeof...(Us) == sizeof...(Ts) &&  //
             (sus::construct::SafelyConstructibleFromReference<T, U &&> &&
              ... &&
              sus::construct::SafelyConstructibleFromReference<Ts, Us &&>))
  explicit constexpr Tuple(U&& first, Us&&... more) noexcept
      : storage_(::sus::forward<U>(first), ::sus::forward<Us>(more)...) {
    // TODO: Convert the requires check to a static_assert when we can test that
    // with a nocompile test.
    static_assert(
        (sus::construct::SafelyConstructibleFromReference<T, U&&> && ... &&
         sus::construct::SafelyConstructibleFromReference<Ts, Us&&>),
        "Unable to safely convert to a different reference type, as "
        "conversion would produce a reference to a temporary. The Tuple's "
        "parameter type must match the Tuple's stored reference. For example a "
        "`Tuple<const i32&, u32>` can not be constructed from "
        "`const i16&, u32` parameters but it can be constructed from "
        " `i32, u16`.");
  }

  /// sus::mem::Clone trait.
  constexpr Tuple clone() const& noexcept
    requires((::sus::mem::CloneOrRef<T> && ... && ::sus::mem::CloneOrRef<Ts>) &&
             !(::sus::mem::CopyOrRef<T> && ... && ::sus::mem::CopyOrRef<Ts>))
  {
    auto f = [this]<size_t... Is>(std::index_sequence<Is...>) {
      return Tuple(::sus::mem::clone_or_forward<T>(
          __private::find_tuple_storage<Is>(storage_).at())...);
    };
    return f(std::make_index_sequence<1u + sizeof...(Ts)>());
  }

  /// Converts from `Tuple<X', Y', Z'>` to `Tuple<X, Y, Z>` when `X'`, `Y'`,
  /// and `Z'` can be converted to `X`, `Y`, and `Z`.
  /// #[doc.overloads=ctor.convert]
  template <class U, class... Us>
    requires(
        (std::convertible_to<const U&, T> && ... &&
         std::convertible_to<const Us&, Ts>) &&  //
        sizeof...(Ts) == sizeof...(Us) &&        //
        (::sus::construct::SafelyConstructibleFromReference<T, const U&> &&
         ... &&
         ::sus::construct::SafelyConstructibleFromReference<Ts, const Us&>))
  constexpr Tuple(const Tuple<U, Us...>& tuple) noexcept
      : Tuple(UNPACK, tuple, std::make_index_sequence<1u + sizeof...(Ts)>()) {
    // TODO: Convert the requires check to a static_assert when we can test that
    // with a nocompile test.
    static_assert(
        (::sus::construct::SafelyConstructibleFromReference<T, const U&> &&
         ... &&
         ::sus::construct::SafelyConstructibleFromReference<Ts, const Us&>),
        "Unable to safely convert to a different reference type, as conversion "
        "would produce a reference to a temporary. The TupleMarker's value "
        "type must match the Tuple's. For example a `Tuple<const i32&, u32>` "
        "can not be constructed from a `Tuple<i16, u32>`, "
        "but it can be constructed from `Tuple<i32, u16>`.");
  }
  /// #[doc.overloads=ctor.convert]
  template <class U, class... Us>
    requires((std::convertible_to<U &&, T> && ... &&
              std::convertible_to<Us &&, Ts>) &&  //
             sizeof...(Ts) == sizeof...(Us) &&    //
             (::sus::construct::SafelyConstructibleFromReference<T, U &&> &&
              ... &&
              ::sus::construct::SafelyConstructibleFromReference<Ts, Us &&>))
  constexpr Tuple(Tuple<U, Us...>&& tuple) noexcept
      : Tuple(UNPACK, ::sus::move(tuple),
              std::make_index_sequence<1u + sizeof...(Ts)>()) {
    // TODO: Convert the requires check to a static_assert when we can test that
    // with a nocompile test.
    static_assert(
        (::sus::construct::SafelyConstructibleFromReference<T, U&&> && ... &&
         ::sus::construct::SafelyConstructibleFromReference<Ts, Us&&>),
        "Unable to safely convert to a different reference type, as conversion "
        "would produce a reference to a temporary. The TupleMarker's value "
        "type must match the Tuple's. For example a `Tuple<const i32&, u32>` "
        "can not be constructed from a `Tuple<i16, u32>`, "
        "but it can be constructed from `Tuple<i32, u16>`.");
  }

  /// Gets a const reference to the `I`th element in the tuple.
  template <size_t I>
    requires(I <= sizeof...(Ts))
  constexpr inline const auto& at() const& noexcept {
    return __private::find_tuple_storage<I>(storage_).at();
  }
  // Disallows getting a reference to temporary Tuple.
  template <size_t I>
  constexpr inline const auto& at() && = delete;

  /// Gets a mutable reference to the `I`th element in the tuple.
  template <size_t I>
    requires(I <= sizeof...(Ts))
  constexpr inline auto& at_mut() & noexcept {
    return __private::find_tuple_storage_mut<I>(storage_).at_mut();
  }

  /// Removes the `I`th element from the tuple, leaving the Tuple in a
  /// moved-from state where it should no longer be used.
  template <size_t I>
    requires(I <= sizeof...(Ts))
  constexpr inline decltype(auto) into_inner() && noexcept {
    return ::sus::move(__private::find_tuple_storage_mut<I>(storage_))
        .into_inner();
  }

  /// Satisfies the [`Eq`]($sus::cmp::Eq) concept if the types inside satisfy
  /// `Eq`.
  ///
  /// # Implementation Note
  /// The non-template overload allows tuple() marker types to convert to
  /// Tuple for comparison.
  friend constexpr bool operator==(const Tuple& l, const Tuple& r) noexcept
    requires((::sus::cmp::Eq<T> && ... && ::sus::cmp::Eq<Ts>))
  {
    return __private::storage_eq(
        l.storage_, r.storage_, std::make_index_sequence<1u + sizeof...(Ts)>());
  }
  template <class U, class... Us>
    requires(sizeof...(Us) == sizeof...(Ts) &&
             (::sus::cmp::Eq<T, U> && ... && ::sus::cmp::Eq<Ts, Us>))
  friend constexpr bool operator==(const Tuple& l,
                                   const Tuple<U, Us...>& r) noexcept {
    return __private::storage_eq(
        l.storage_, r.storage_, std::make_index_sequence<1u + sizeof...(Ts)>());
  }

  /// Compares two Tuples.
  ///
  /// Satisfies sus::cmp::StrongOrd<Tuple<...>> if sus::cmp::StrongOrd<...>.
  /// Satisfies sus::cmp::Ord<Tuple<...>> if sus::cmp::Ord<...>.
  /// Satisfies sus::cmp::PartialOrd<Tuple<...>> if sus::cmp::PartialOrd<...>.
  ///
  /// The non-template overloads allow tuple() marker types to convert to
  /// Tuple for comparison.
  //
  // sus::cmp::StrongOrd<Tuple<U...>> trait.
  friend constexpr std::strong_ordering operator<=>(const Tuple& l,
                                                    const Tuple& r) noexcept
    requires((::sus::cmp::StrongOrd<T> && ... && ::sus::cmp::StrongOrd<Ts>))
  {
    return __private::storage_cmp(
        std::strong_ordering::equal, l.storage_, r.storage_,
        std::make_index_sequence<1u + sizeof...(Ts)>());
  }
  template <class U, class... Us>
    requires(sizeof...(Us) == sizeof...(Ts) &&
             (::sus::cmp::StrongOrd<T, U> && ... &&
              ::sus::cmp::StrongOrd<Ts, Us>))
  friend constexpr std::strong_ordering operator<=>(
      const Tuple& l, const Tuple<U, Us...>& r) noexcept {
    return __private::storage_cmp(
        std::strong_ordering::equal, l.storage_, r.storage_,
        std::make_index_sequence<1u + sizeof...(Ts)>());
  }

  // sus::cmp::Ord<Tuple<U...>> trait.
  friend constexpr std::weak_ordering operator<=>(const Tuple& l,
                                                  const Tuple& r) noexcept
    requires(!(::sus::cmp::StrongOrd<T> && ... && ::sus::cmp::StrongOrd<Ts>) &&
             (::sus::cmp::Ord<T> && ... && ::sus::cmp::Ord<Ts>))
  {
    return __private::storage_cmp(
        std::weak_ordering::equivalent, l.storage_, r.storage_,
        std::make_index_sequence<1u + sizeof...(Ts)>());
  }
  template <class U, class... Us>
    requires(sizeof...(Us) == sizeof...(Ts) &&
             !(::sus::cmp::StrongOrd<T, U> && ... &&
               ::sus::cmp::StrongOrd<Ts, Us>) &&
             (::sus::cmp::Ord<T, U> && ... && ::sus::cmp::Ord<Ts, Us>))
  friend constexpr std::weak_ordering operator<=>(
      const Tuple& l, const Tuple<U, Us...>& r) noexcept {
    return __private::storage_cmp(
        std::weak_ordering::equivalent, l.storage_, r.storage_,
        std::make_index_sequence<1u + sizeof...(Ts)>());
  }

  // sus::cmp::PartialOrd<Tuple<U...>> trait.
  friend constexpr std::partial_ordering operator<=>(const Tuple& l,
                                                     const Tuple& r) noexcept
    requires(!(::sus::cmp::Ord<T> && ... && ::sus::cmp::Ord<Ts>) &&
             (::sus::cmp::PartialOrd<T> && ... && ::sus::cmp::PartialOrd<Ts>))
  {
    return __private::storage_cmp(
        std::partial_ordering::equivalent, l.storage_, r.storage_,
        std::make_index_sequence<1u + sizeof...(Ts)>());
  }
  template <class U, class... Us>
    requires(sizeof...(Us) == sizeof...(Ts) &&
             !(::sus::cmp::Ord<T, U> && ... && ::sus::cmp::Ord<Ts, Us>) &&
             (::sus::cmp::PartialOrd<T, U> && ... &&
              ::sus::cmp::PartialOrd<Ts, Us>))
  friend constexpr std::partial_ordering operator<=>(
      const Tuple& l, const Tuple<U, Us...>& r) noexcept {
    return __private::storage_cmp(
        std::partial_ordering::equivalent, l.storage_, r.storage_,
        std::make_index_sequence<1u + sizeof...(Ts)>());
  }

  /// Satisfies sus::iter::Extend for a Tuple of collections that each satisfies
  /// Extend for its position-relative type in the iterator of tuples.
  ///
  /// The tuple this is called on is a set of collections. The iterable passed
  /// in as an argument yields tuples of items that will be appended to the
  /// collections.
  ///
  /// The item types in the argument can not be deduced, so they must be
  /// explicitly specified by the caller, such as:
  /// ```cpp
  /// collections.extend<i32, std::string>(iter_over_tuples_i32_string());
  /// ```
  ///
  /// Allows to `extend` a tuple of collections that also implement `Extend`.
  ///
  /// See also: `IteratorBase::unzip`.
  template <class U, class... Us>
    requires(sizeof...(Us) == sizeof...(Ts) &&
             (::sus::iter::Extend<T, U> && ... && ::sus::iter::Extend<Ts, Us>))
  constexpr void extend(
      ::sus::iter::IntoIterator<Tuple<U, Us...>> auto&& ii) noexcept
    requires(::sus::mem::IsMoveRef<decltype(ii)>)
  {
    for (Tuple<U, Us...>&& item : ::sus::move(ii).into_iter()) {
      auto f = [this]<size_t... Is>(Tuple<U, Us...>&& item,
                                    std::index_sequence<Is...>) mutable {
        // Alias to avoid packs in the fold expression.
        using Items = Tuple<U, Us...>;
        // TODO: Consider adding extend_one() to the Extend concept, which can
        // take I instead of an Option<I>.
        (..., at_mut<Is>().extend(
                  ::sus::option::Option<typename Items::template IthType<Is>>(
                      ::sus::move(item).template into_inner<Is>())));
      };
      f(::sus::move(item), std::make_index_sequence<1u + sizeof...(Ts)>());
    }
  }

  _sus_format_to_stream(Tuple);

 private:
  template <class U, class... Us>
  friend class Tuple;

  enum UnpackContructor { UNPACK };
  template <class U, class... Us, size_t... Is>
  explicit constexpr Tuple(UnpackContructor, const Tuple<U, Us...>& tuple,
                           std::index_sequence<Is...>) noexcept
      : Tuple(__private::find_tuple_storage<Is>(tuple.storage_).at()...) {}
  template <class U, class... Us, size_t... Is>
  explicit constexpr Tuple(UnpackContructor, Tuple<U, Us...>&& tuple,
                           std::index_sequence<Is...>) noexcept
      : Tuple(::sus::move(__private::find_tuple_storage_mut<Is>(tuple.storage_))
                  .into_inner()...) {}

  /// Storage for the tuple elements.
  using Storage = __private::TupleStorage<T, Ts...>;

  template <size_t I>
  using IthType = ::sus::choice_type::__private::PackIth<I, T, Ts...>;

  // The use of `[[no_unique_address]]` allows the tail padding of of the
  // `storage_` to be used in structs that request to do so by putting
  // `[[no_unique_address]]` on the Tuple. For example, Choice does this with
  // its internal Tuples to put its tag inside the Tuples' storage when
  // possible.
  [[_sus_no_unique_address]] Storage storage_;

  sus_class_trivially_relocatable_if_types(::sus::marker::unsafe_fn, T, Ts...);
};

template <class... Ts>
Tuple(Ts&&...) -> Tuple<std::remove_cvref_t<Ts>...>;

// Support for structured binding.
template <size_t I, class... Ts>
constexpr decltype(auto) get(const Tuple<Ts...>& t) noexcept {
  return t.template at<I>();
}
template <size_t I, class... Ts>
constexpr decltype(auto) get(Tuple<Ts...>& t) noexcept {
  return t.template at_mut<I>();
}
template <size_t I, class... Ts>
constexpr decltype(auto) get(Tuple<Ts...>&& t) noexcept {
  // We explicitly don't move-from `t` to call `t.into_inner()` as this `get()`
  // function will be called for each member of `t` when making structured
  // bindings from an rvalue Tuple.
  return static_cast<decltype(::sus::move(t).template into_inner<I>())>(
      t.template at_mut<I>());
}

namespace __private {
template <class... Ts>
struct [[nodiscard]] TupleMarker {
  explicit constexpr TupleMarker(Tuple<Ts&&...>&& values)
      : values(::sus::move(values)) {}

  Tuple<Ts&&...> values;

  // Gtest macros force evaluation against a const reference.
  // https://github.com/google/googletest/issues/4350
  //
  // So we allow constructing the target type from a const ref `value`. This
  // can perform explicit conversions which isn't correct but is required in
  // order to be able to call this method twice, because `value` itself may not
  // be copyable (marker types are not) but construction from const is more
  // likely.
  template <class... Us>
  _sus_pure inline constexpr operator Tuple<Us...>() const& noexcept {
    static_assert(
        (... &&
         std::convertible_to<::sus::mem::remove_rvalue_reference<Ts>, Us>),
        "TupleMarker(T) can't convert const T& to U for Tuple<U>. "
        "Note that this is a test-only code path for Gtest support. "
        "Typically the T object is consumed as an rvalue, consider using "
        "EXPECT_TRUE(a == b) if needed.");
    static_assert((... && std::convertible_to<Ts&&, Us>),
                  "TupleMarker(T) can't convert T to U for Tuple<U>");
    static_assert(
        (... &&
         ::sus::construct::SafelyConstructibleFromReference<Us, const Ts&>),
        "Unable to safely convert to a different reference type, as conversion "
        "would produce a reference to a temporary. The TupleMarker's value "
        "type must match the Tuple's. For example a `Tuple<const i32&, u32>` "
        "can not be constructed from a TupleMarker holding `const i16&, u32`, "
        "but it can be constructed from `i32&&, u16`.");
    auto make_tuple =
        [this]<size_t... Is>(std::integer_sequence<size_t, Is...>) {
          return Tuple<Us...>(Us(values.template at<Is>())...);
        };
    return make_tuple(std::make_integer_sequence<size_t, sizeof...(Ts)>());
  }

  template <class... Us>
  inline constexpr operator Tuple<Us...>() && noexcept {
    static_assert((... && std::convertible_to<Ts&&, Us>),
                  "TupleMarker(T) can't convert T to U for Tuple<U>");
    static_assert(
        (... && ::sus::construct::SafelyConstructibleFromReference<Us, Ts&&>),
        "Unable to safely convert to a different reference type, as conversion "
        "would produce a reference to a temporary. The TupleMarker's value "
        "type must match the Tuple's. For example a `Tuple<const i32&, u32>` "
        "can not be constructed from a TupleMarker holding `const i16&, u32`, "
        "but it can be constructed from `i32&&, u16`.");
    auto make_tuple = [this]<size_t... Is>(
                          std::integer_sequence<size_t, Is...>) {
      return Tuple<Us...>(::sus::forward<Ts>(values.template at_mut<Is>())...);
    };
    return make_tuple(std::make_integer_sequence<size_t, sizeof...(Ts)>());
  }

  // Doesn't copy or move. `TupleMarker` should only be used as a temporary.
  TupleMarker(const TupleMarker&) = delete;
  TupleMarker& operator=(const TupleMarker&) = delete;
};

}  // namespace __private

/// Used to construct a [`Tuple`]($sus::tuple_type::Tuple) with the
/// parameters as its values.
///
/// Calling `tuple()` produces a hint to make a [`Tuple`](
/// $sus::tuple_type::Tuple) but does not actually
/// construct [`Tuple`]($sus::tuple_type::Tuple), as the types in the
/// [`Tuple`]($sus::tuple_type::Tuple) are not known yet here and are not
/// assumed to be the same as the parameters.
///
/// The marker type holds a refernce to each parameter, allowing it to construct
/// a [`Tuple`]($sus::tuple_type::Tuple) which holds references correctly. The
/// marker must be consumed immediately into a [`Tuple`](
/// $sus::tuple_type::Tuple) variable, which is normally done by using the
/// `tuple` function as a function argument or in a return statement.
template <class... Ts>
  requires(sizeof...(Ts) > 0)
[[nodiscard]] inline constexpr auto tuple(
    Ts&&... vs sus_lifetimebound) noexcept {
  return __private::TupleMarker<Ts...>(
      ::sus::tuple_type::Tuple<Ts&&...>(::sus::forward<Ts>(vs)...));
}

}  // namespace sus::tuple_type

namespace std {
template <class... Types>
struct tuple_size<::sus::tuple_type::Tuple<Types...>> {
  static constexpr size_t value = sizeof...(Types);
};

template <size_t I, class T, class... Types>
struct tuple_element<I, ::sus::tuple_type::Tuple<T, Types...>> {
  using type = tuple_element<I - 1, ::sus::tuple_type::Tuple<Types...>>::type;
};

template <class T, class... Types>
struct tuple_element<0, ::sus::tuple_type::Tuple<T, Types...>> {
  using type = T;
};

}  // namespace std

// fmt support.
template <class... Types, class Char>
struct fmt::formatter<::sus::tuple_type::Tuple<Types...>, Char> {
  template <class ParseContext>
  constexpr auto parse(ParseContext& ctx) {
    return ctx.begin();
  }

  template <class FormatContext>
  constexpr auto format(const ::sus::tuple_type::Tuple<Types...>& tuple,
                        FormatContext& ctx) const {
    auto out = ctx.out();
    *out++ = static_cast<Char>('(');
    ctx.advance_to(out);
    out =
        format_tuple(tuple, ctx, std::make_index_sequence<sizeof...(Types)>());
    *out++ = static_cast<Char>(')');
    return out;
  }

 private:
  template <size_t... Is, class FormatContext>
  static auto format_tuple(const ::sus::tuple_type::Tuple<Types...>& tuple,
                           FormatContext& ctx, std::index_sequence<Is...>) {
    (..., format_value<Is>(tuple, ctx));
    return ctx.out();
  }

  template <size_t I, class FormatContext>
  static void format_value(const ::sus::tuple_type::Tuple<Types...>& tuple,
                           FormatContext& ctx) {
    using ValueFormatter =
        ::sus::string::__private::AnyFormatter<decltype(tuple.template at<I>()),
                                               Char>;
    auto out = ValueFormatter().format(tuple.template at<I>(), ctx);
    if constexpr (I < sizeof...(Types) - 1) {
      *out++ = static_cast<Char>(',');
      *out++ = static_cast<Char>(' ');
      ctx.advance_to(out);
    }
  }
};

// Promote Tuple into the `sus` namespace.
namespace sus {
using ::sus::tuple_type::Tuple;
using ::sus::tuple_type::tuple;
}  // namespace sus

#undef SUS_CONFIG_TUPLE_USE_AFTER_MOVE
