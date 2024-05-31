// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <mutex>
#include <shared_mutex>

#include "glaze/glaze.hpp"

namespace glz::repe
{
   constexpr uint8_t notify = 0b00000001;
   constexpr uint8_t empty = 0b00000010;

   // Note: The method and id are at the top of the class for easier initialization
   // The order in the serialized message follows the REPE specification
   struct header final
   {
      std::string_view method = ""; // the RPC method (JSON pointer path) to call or member to access/assign (GET/POST)
      std::variant<std::monostate, uint64_t, std::string_view> id{}; // an identifier
      static constexpr uint8_t version = 0; // the REPE version
      uint8_t error = 0; // 0 denotes no error (boolean: 0 or 1)

      // Action: These booleans are packed into the a uint8_t action in the REPE header
      bool notify{}; // no response returned
      bool empty{}; // the body should be ignored (considered empty)
   };

   template <class T>
   concept is_header = std::same_as<std::decay_t<T>, header>;
}

template <>
struct glz::meta<glz::repe::header>
{
   using T = glz::repe::header;
   static constexpr sv name = "glz::repe::header"; // Hack to fix MSVC (shouldn't be needed)
   static constexpr auto read_action = [](T& self, const uint8_t input) {
      self.notify = input & 0b00000001;
      self.empty = input & 0b00000010;
   };
   static constexpr auto write_action = [](const T& self) {
      uint8_t action{};
      action |= self.notify;
      action |= (self.empty << 1);
      return action;
   };
   static constexpr auto value =
      glz::array(&T::version, &T::error, custom<read_action, write_action>, &T::method, &T::id);
};

namespace glz::repe
{
   struct error_e
   {
      static constexpr int32_t no_error = 0;
      static constexpr int32_t server_error_lower = -32000;
      static constexpr int32_t server_error_upper = -32099;
      static constexpr int32_t invalid_request = -32600;
      static constexpr int32_t method_not_found = -32601;
      static constexpr int32_t invalid_params = -32602;
      static constexpr int32_t internal = -32603;
      static constexpr int32_t parse_error = -32700;
   };

   struct error_t final
   {
      int32_t code = error_e::no_error;
      std::string message = "";

      operator bool() const noexcept { return bool(code); }

      struct glaze
      {
         using T = error_t;
         static constexpr auto value = glz::array(&T::code, &T::message);
      };
   };

   struct state final
   {
      const std::string_view message{};
      repe::header& header;
      std::string& buffer;
      error_t& error;
   };

   template <class T>
   concept is_state = std::same_as<std::decay_t<T>, state>;

   namespace detail
   {
      struct string_hash
      {
         using is_transparent = void;
         [[nodiscard]] size_t operator()(const char* txt) const { return std::hash<std::string_view>{}(txt); }
         [[nodiscard]] size_t operator()(std::string_view txt) const { return std::hash<std::string_view>{}(txt); }
         [[nodiscard]] size_t operator()(const std::string& txt) const { return std::hash<std::string>{}(txt); }
      };
   }

   inline auto& get_shared_mutex()
   {
      static std::shared_mutex mtx{};
      return mtx;
   }

   // returns 0 on error
   template <opts Opts, class Value>
   size_t read_params(Value&& value, auto&& state, auto&& response)
   {
      glz::context ctx{};
      auto [b, e] = read_iterators<Opts>(ctx, state.message);
      if (bool(ctx.error)) [[unlikely]] {
         return 0;
      }
      auto start = b;

      glz::detail::read<Opts.format>::template op<Opts>(std::forward<Value>(value), ctx, b, e);

      if (bool(ctx.error)) {
         parse_error ec{ctx.error, size_t(b - start), ctx.includer_error};
         write<Opts>(std::forward_as_tuple(header{.error = true},
                                           error_t{error_e::parse_error, format_error(ec, state.message)}),
                     response);
         return 0;
      }

      return size_t(b - start);
   }

   template <opts Opts, class Value>
   void write_response(Value&& value, is_state auto&& state)
   {
      if (state.error) {
         write<Opts>(std::forward_as_tuple(header{.error = true}, state.error), state.buffer);
      }
      else {
         state.header.empty = false; // we are writing a response
         write<Opts>(std::forward_as_tuple(state.header, std::forward<Value>(value)), state.buffer);
      }
   }

   template <opts Opts>
   void write_response(is_state auto&& state)
   {
      if (state.error) {
         write<Opts>(std::forward_as_tuple(header{.error = true}, state.error), state.buffer);
      }
      else {
         state.header.notify = false;
         state.header.empty = true;
         write<Opts>(std::forward_as_tuple(state.header, nullptr), state.buffer);
      }
   }

   struct ignore_result final
   {};

   template <opts Opts, class Result>
   [[nodiscard]] error_t decode_response(Result&& result, auto& buffer)
   {
      repe::header h;
      context ctx{};
      auto [b, e] = read_iterators<Opts>(ctx, buffer);
      if (bool(ctx.error)) [[unlikely]] {
         return error_t{error_e::parse_error};
      }
      auto start = b;

      auto handle_error = [&](auto& it) {
         ctx.error = error_code::syntax_error;
         parse_error pe{ctx.error, size_t(it - start), ctx.includer_error};
         return error_t{error_e::parse_error, format_error(pe, buffer)};
      };

      if (*b == '[') {
         ++b;
      }
      else {
         return handle_error(b);
      }

      glz::detail::read<Opts.format>::template op<Opts>(h, ctx, b, e);

      if (bool(ctx.error)) {
         parse_error pe{ctx.error, size_t(b - start), ctx.includer_error};
         return {error_e::parse_error, format_error(pe, buffer)};
      }

      if (*b == ',') {
         ++b;
      }
      else {
         return handle_error(b);
      }

      if (h.error) {
         error_t error{};
         glz::detail::read<Opts.format>::template op<Opts>(error, ctx, b, e);
         return error;
      }

      if constexpr (!std::same_as<std::decay_t<Result>, ignore_result>) {
         glz::detail::read<Opts.format>::template op<Opts>(result, ctx, b, e);

         if (bool(ctx.error)) {
            parse_error pe{ctx.error, size_t(b - start), ctx.includer_error};
            return {error_e::parse_error, format_error(pe, buffer)};
         }
      }

      return {};
   }

   template <opts Opts>
   [[nodiscard]] error_t decode_response(auto& buffer)
   {
      return decode_response<Opts>(ignore_result{}, buffer);
   }

   template <opts Opts, class Value>
   inline auto request(const header& header, Value&& value)
   {
      return glz::write<Opts>(std::forward_as_tuple(header, std::forward<Value>(value)));
   }

   template <opts Opts, class Value>
   inline auto request(const header& header, Value&& value, auto& buffer)
   {
      return glz::write<Opts>(std::forward_as_tuple(header, std::forward<Value>(value)), buffer);
   }

   inline auto request_json(header&& header)
   {
      header.empty = true; // because no value provided
      return glz::write_json(std::forward_as_tuple(header, nullptr));
   }

   inline auto request_binary(header&& header)
   {
      header.empty = true; // because no value provided
      return glz::write_binary(std::forward_as_tuple(header, nullptr));
   }

   inline auto request_json(const header& h)
   {
      repe::header copy = h;
      copy.empty = true; // because no value provided
      return request_json(std::move(copy));
   }

   inline auto request_binary(const header& h)
   {
      repe::header copy = h;
      copy.empty = true; // because no value provided
      return request_binary(std::move(copy));
   }

   template <class Value>
   inline auto request_json(const header& header, Value&& value)
   {
      return glz::write_json(std::forward_as_tuple(header, std::forward<Value>(value)));
   }

   template <class Value>
   inline auto request_binary(const header& header, Value&& value)
   {
      return glz::write_binary(std::forward_as_tuple(header, std::forward<Value>(value)));
   }

   template <class Value>
   inline void request_json(const header& header, Value&& value, auto& buffer)
   {
      glz::write_json(std::forward_as_tuple(header, std::forward<Value>(value)), buffer);
   }

   template <class Value>
   inline void request_binary(const header& header, Value&& value, auto& buffer)
   {
      glz::write_binary(std::forward_as_tuple(header, std::forward<Value>(value)), buffer);
   }

   // DESIGN NOTE: It might appear that we are locking ourselves into a poor design choice by using a runtime
   // std::unordered_map. However, we can actually improve this in the future by allowing glz::meta specialization on
   // the server to list out the method names and allow us to build a compile time map prior to function registration.
   // This is made possible by constinit, and the same approach used for compile time maps and struct reflection.
   // This will then be an opt-in performance improvement where a bit more code must be written by the user to
   // list the method names.

   // This server is designed to be lightweight, and meant to be constructed on a per client basis
   // This server does not support adding methods from RPC calls or adding methods once RPC calls can be made
   // Each instance of this server is expected to be accessed by a single thread, so a single std::string response
   // buffer is used.
   template <opts Opts = opts{}>
   struct registry
   {
      using procedure = std::function<void(state&&)>; // RPC method
      std::unordered_map<std::string_view, procedure, detail::string_hash, std::equal_to<>> methods;

      std::string response{};

      error_t error{};

      static constexpr std::string_view empty_path = "";

      template <const std::string_view& root = empty_path, class T, const std::string_view& parent = root>
         requires(glz::detail::glaze_object_t<T> || glz::detail::reflectable<T>)
      void on(T& value)
      {
         using namespace glz::detail;
         static constexpr auto N = reflection_count<T>;

         [[maybe_unused]] decltype(auto) t = [&] {
            if constexpr (reflectable<T>) {
               return to_tuple(value);
            }
            else {
               return nullptr;
            }
         }();

         if constexpr (parent == root && (glaze_object_t<T> || reflectable<T>)) {
            // build read/write calls to the top level object
            methods[root] = [this, &value](repe::state&& state) {
               if (not state.header.empty) {
                  if (read_params<Opts>(value, state, response) == 0) {
                     return;
                  }
               }

               if (state.header.notify) {
                  return;
               }

               if (state.header.empty) {
                  write_response<Opts>(value, state);
               }
               else {
                  write_response<Opts>(state);
               }
            };
         }

         for_each<N>([&](auto I) {
            using Element = glaze_tuple_element<I, N, T>;
            static constexpr std::string_view full_key = [&] {
               if constexpr (parent == empty_path) {
                  return join_v<chars<"/">, key_name<I, T, Element::use_reflection>>;
               }
               else {
                  return join_v<parent, chars<"/">, key_name<I, T, Element::use_reflection>>;
               }
            }();

            using E = typename Element::type;
            decltype(auto) func = [&]() -> decltype(auto) {
               if constexpr (reflectable<T>) {
                  return std::get<I>(t);
               }
               else {
                  return get_member(value, get<Element::member_index>(get<I>(meta_v<T>)));
               }
            }();

            // This logic chain should match glz::cli_menu
            using Func = decltype(func);
            if constexpr (std::is_invocable_v<Func>) {
               using Result = std::invoke_result_t<Func>;
               if constexpr (std::same_as<Result, void>) {
                  methods[full_key] = [callback = func](repe::state&& state) mutable {
                     callback();
                     if (state.header.notify) {
                        return;
                     }
                     write_response<Opts>(state);
                  };
               }
               else {
                  methods[full_key] = [result = Result{}, callback = func](repe::state&& state) mutable {
                     result = callback();
                     if (state.header.notify) {
                        return;
                     }
                     write_response<Opts>(result, state);
                  };
               }
            }
            else if constexpr (is_invocable_concrete<std::remove_cvref_t<Func>>) {
               using Tuple = invocable_args_t<std::remove_cvref_t<Func>>;
               constexpr auto N = glz::tuple_size_v<Tuple>;
               static_assert(N == 1, "Only one input is allowed for your function");

               using Params = glz::tuple_element_t<0, Tuple>;
               using Result = std::invoke_result_t<Func, Params>;

               methods[full_key] = [this, params = std::decay_t<Params>{}, result = std::decay_t<Result>{},
                                    callback = func](repe::state&& state) mutable {
                  // no need to lock locals
                  if (read_params<Opts>(params, state, response) == 0) {
                     return;
                  }
                  result = callback(params);
                  if (state.header.notify) {
                     return;
                  }
                  write_response<Opts>(result, state);
               };
            }
            else if constexpr (glaze_object_t<E> || reflectable<E>) {
               on<root, std::decay_t<E>, full_key>(get_member(value, func));

               // build read/write calls to the object as a variable
               methods[full_key] = [this, &func](repe::state&& state) {
                  if (not state.header.empty) {
                     if (read_params<Opts>(func, state, response) == 0) {
                        return;
                     }
                  }

                  if (state.header.notify) {
                     return;
                  }

                  if (state.header.empty) {
                     write_response<Opts>(func, state);
                  }
                  else {
                     write_response<Opts>(state);
                  }
               };
            }
            else if constexpr (!std::is_lvalue_reference_v<Func>) {
               // For glz::custom, glz::manage, etc.
               methods[full_key] = [this, func](repe::state&& state) mutable {
                  if (not state.header.empty) {
                     if (read_params<Opts>(func, state, response) == 0) {
                        return;
                     }
                  }

                  if (state.header.notify) {
                     return;
                  }

                  if (state.header.empty) {
                     write_response<Opts>(func, state);
                  }
                  else {
                     write_response<Opts>(state);
                  }
               };
            }
            else {
               static_assert(std::is_lvalue_reference_v<Func>);

               if constexpr (std::is_member_function_pointer_v<std::decay_t<Func>>) {
                  using F = std::decay_t<Func>;
                  using Ret = typename return_type<F>::type;
                  using Tuple = typename inputs_as_tuple<F>::type;
                  constexpr auto n_args = glz::tuple_size_v<Tuple>;
                  if constexpr (std::is_void_v<Ret>) {
                     if constexpr (n_args == 0) {
                        methods[full_key] = [&value, &func](repe::state&& state) {
                           (value.*func)();

                           if (state.header.notify) {
                              return;
                           }

                           state.header.empty = false;
                           write_response<Opts>(state);
                        };
                     }
                     else if constexpr (n_args == 1) {
                        using Input = std::decay_t<glz::tuple_element_t<0, Tuple>>;
                        methods[full_key] = [this, &value, &func, input = Input{}](repe::state&& state) mutable {
                           if (not state.header.empty) {
                              if (read_params<Opts>(input, state, response) == 0) {
                                 return;
                              }
                           }

                           (value.*func)(input);

                           if (state.header.notify) {
                              return;
                           }

                           state.header.empty = false;
                           write_response<Opts>(state);
                        };
                     }
                     else {
                        static_assert(false_v<Func>, "function cannot have more than one input");
                     }
                  }
                  else {
                     // Member function pointers
                     if constexpr (n_args == 0) {
                        methods[full_key] = [&value, &func](repe::state&& state) {
                           auto result = (value.*func)();

                           if (state.header.notify) {
                              return;
                           }

                           if (state.header.empty) {
                              write_response<Opts>(result, state);
                           }
                           else {
                              write_response<Opts>(state);
                           }
                        };
                     }
                     else if constexpr (n_args == 1) {
                        using Input = std::decay_t<glz::tuple_element_t<0, Tuple>>;
                        methods[full_key] = [this, &value, &func, input = Input{}](repe::state&& state) mutable {
                           if (not state.header.empty) {
                              if (read_params<Opts>(input, state, response) == 0) {
                                 return;
                              }
                           }

                           auto result = (value.*func)(input);

                           if (state.header.notify) {
                              return;
                           }

                           if (state.header.empty) {
                              write_response<Opts>(result, state);
                           }
                           else {
                              write_response<Opts>(state);
                           }
                        };
                     }
                     else {
                        static_assert(false_v<Func>, "function cannot have more than one input");
                     }
                  }
               }
               else {
                  // this is a variable and not a function, so we build RPC read/write calls
                  methods[full_key] = [this, &func](repe::state&& state) {
                     if (not state.header.empty) {
                        if (read_params<Opts>(func, state, response) == 0) {
                           return;
                        }
                     }

                     if (state.header.notify) {
                        return;
                     }

                     if (state.header.empty) {
                        write_response<Opts>(func, state);
                     }
                     else {
                        write_response<Opts>(state);
                     }
                  };
               }
            }
         });
      }

      // TODO: Implement JSON Pointer lock free path access when paths do not collide
      /*template <class Params, class Result, class Callback>
      void on(const sv name, Params&& params, Result&& result, Callback&& callback) {

      }*/

      template <class Value>
      bool call(const header& header)
      {
         return call(request<Opts>(header));
      }

      template <class Value>
      bool call(const header& header, Value&& value)
      {
         return call(request<Opts>(header, std::forward<Value>(value)));
      }

      template <class Value>
      bool call(const header& header, Value&& value, auto& buffer)
      {
         return call(request<Opts>(header, std::forward<Value>(value), buffer));
      }

      // returns true if there is a result to send (not a notification)
      bool call(const sv msg)
      {
         context ctx{};
         auto [b, e] = read_iterators<Opts>(ctx, msg);
         if (bool(ctx.error)) [[unlikely]] {
            return error_t{error_e::parse_error};
         }
         auto start = b;

         auto handle_error = [&](auto& it) {
            ctx.error = error_code::syntax_error;
            parse_error pe{ctx.error, size_t(it - start), ctx.includer_error};
            write<Opts>(
               std::forward_as_tuple(header{.error = true}, error_t{error_e::parse_error, format_error(pe, msg)}),
               response);
         };

         header h{};

         if constexpr (Opts.format == json) {
            if (*b == '[') {
               ++b;
            }
            else {
               handle_error(b);
               return !h.notify;
            }
         }
         else {
            if (*b == glz::tag::generic_array) {
               ++b; // skip the tag
               const auto n = glz::detail::int_from_compressed(ctx, b, e);
               if (bool(ctx.error) || (n != 2)) [[unlikely]] {
                  handle_error(b);
                  return !h.notify;
               }
            }
            else {
               handle_error(b);
               return !h.notify;
            }
         }

         glz::detail::read<Opts.format>::template op<Opts>(h, ctx, b, e);

         if (bool(ctx.error)) [[unlikely]] {
            parse_error pe{ctx.error, size_t(b - start), ctx.includer_error};
            response = format_error(pe, msg);
            return !h.notify;
         }

         if constexpr (Opts.format == json) {
            if (*b == ',') {
               ++b;
            }
            else {
               handle_error(b);
               return !h.notify;
            }
         }

         if (auto it = methods.find(h.method); it != methods.end()) {
            const sv body = msg.substr(size_t(b - start));
            it->second(state{body, h, response, error}); // handle the body
         }
         else {
            write<Opts>(std::forward_as_tuple(header{.error = true}, error_t{error_e::method_not_found}), response);
         }

         return !h.notify;
      }
   };
}
