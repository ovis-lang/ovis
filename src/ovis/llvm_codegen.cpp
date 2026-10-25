#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <set>

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <ovis/codegen/llvm_codegen.hpp>
#include <ovis/codegen/runtime.hpp>

namespace ovis::codegen {

namespace {

using core::base_type;
using core::expression;
using core::statement;
using core::value_type;

auto host_target_machine() -> std::unique_ptr<llvm::TargetMachine>;
auto set_target(llvm::Module &p_module, const llvm::TargetMachine &p_machine)
    -> void;

class generator {
  struct variable {
    llvm::Value *address;
    value_type type;
  };
  // What leaving a block does besides running its `defer`red statements: a
  // `with` block gives its memory back (and frees it, if the block made it).
  struct exit_action {
    const statement *deferred = nullptr;
    llvm::Value *memory = nullptr;
    bool frees = false;
    // The line of the `with`, which is where the memory is given back.
    std::size_t line = 0;
  };
  // A bare block being generated: where `leave` goes, how many cleanup
  // frames were open outside it, and the variable it gives a value to.
  struct block_exit {
    string_type label;
    llvm::BasicBlock *end_block;
    std::size_t frame_depth;
    // Found when the block starts: a name declared inside may hide it.
    variable *gives;
    string_type name;
  };
  struct loop_targets {
    llvm::BasicBlock *continue_block;
    llvm::BasicBlock *break_block;
    // How many cleanup frames were open outside the loop body.
    std::size_t frame_depth;
  };

  const core::program &source;
  llvm::LLVMContext &context;
  std::unique_ptr<llvm::Module> module;
  llvm::IRBuilder<> builder;

  std::map<string_type, llvm::Function *> functions;
  std::map<string_type, const core::c_function *> c_functions;
  // Each record is a struct of its fields, always reached through a pointer.
  std::map<string_type, const core::record *> records;
  std::map<string_type, llvm::StructType *> record_types;
  // Every record starts with a number saying which record it is (its place
  // in `records`), so a value known only by a trait can find its methods.
  std::map<string_type, unsigned> record_tags;
  std::map<string_type, const core::trait *> traits;
  std::map<string_type, const core::trait *> method_owner;
  std::set<std::pair<string_type, string_type>> implemented;
  // A trait method → one function per record, by tag; null where the record
  // doesn't implement the trait.
  std::map<string_type, llvm::GlobalVariable *> dispatch_tables;
  // A trait method only one record answers: its tag and function, called
  // directly.
  std::map<string_type, std::pair<std::size_t, llvm::Function *>>
      single_implementation;
  std::map<string_type, llvm::Function *> record_printers;
  std::map<string_type, variable> globals;
  std::vector<std::map<string_type, variable>> scopes;
  std::vector<loop_targets> loops;
  // One frame per open block (a function's body, `main`, an `if` branch, a
  // loop body): the `defer` statements reached so far, in order. They are
  // copied to every exit of the block.
  std::vector<block_exit> bare_blocks;
  std::vector<std::vector<exit_action>> frames;
  std::map<string_type, llvm::Function *> record_keepers;
  // Checked builds keep the running source line in `ovis_line`.
  bool checked = true;
  // Whether stored values take their variable's name (checked, not the
  // prelude), and the names' text.
  bool naming = false;
  std::map<string_type, llvm::Value *> variable_names;
  // C functions written for Ovis phrases given to C, by phrase and type.
  std::map<string_type, llvm::Function *> thunks;
  llvm::GlobalVariable *line_global = nullptr;
  const std::vector<std::size_t> *current_lines = nullptr;
  std::size_t current_line = 0;
  // In a checked build's function without source lines (the prelude's):
  // the line it was called from.
  llvm::Value *entry_line = nullptr;
  llvm::Function *current_function = nullptr;
  value_type current_return_type;
  bool in_main = false;
  // How whole-number `+`, `-` and `*` treat a result too big for its type:
  // an error (checked builds), or inside `(wrapping …)` and `(saturating
  // …)` its low bits or the nearest value.
  int arithmetic = 0;
  static constexpr int arithmetic_checked = 0;
  static constexpr int arithmetic_wrapping = 1;
  static constexpr int arithmetic_saturating = 2;

  // Types.

  auto type_of(const value_type &p_type) -> llvm::Type * {
    if (p_type.is_list)
      return builder.getPtrTy();
    if (p_type.is_sized())
      return c_llvm_type(builder, *p_type.number_type());
    switch (p_type.base) {
    case base_type::nothing:
      return builder.getVoidTy();
    case base_type::integer:
      return builder.getInt64Ty();
    case base_type::real:
      return builder.getDoubleTy();
    case base_type::boolean:
      return builder.getInt1Ty();
    case base_type::string:
    case base_type::pointer:
    case base_type::memory:
    case base_type::record:
    case base_type::map:
      return builder.getPtrTy();
    case base_type::unknown:
      break;
    }
    throw error("Internal compiler error: a type was not worked out.");
  }

  // A list's items as they are kept: a C array's (`(list u8)`) carry their
  // C number type in `pointee`, so they are read and written at its size.
  static auto item_storage(const value_type &p_list) -> value_type {
    auto result = p_list.element();
    if (p_list.inner_lists == 0)
      result.pointee = p_list.pointee;
    return result;
  }
  // The item as Ovis sees it: a number of a size is that number.
  static auto item_value(const value_type &p_item) -> value_type {
    return p_item;
  }

  static auto is_byte(const value_type &p_element) -> bool {
    return p_element.base == base_type::boolean && !p_element.is_list;
  }

  // Booleans are stored as bytes inside lists so the C runtime can read them.
  auto element_storage_type(const value_type &p_element) -> llvm::Type * {
    if (!p_element.is_list && !p_element.pointee.empty() &&
        p_element.base != base_type::map)
      return c_llvm_type(builder, *core::c_number_type(p_element.pointee));
    return is_byte(p_element) ? builder.getInt8Ty() : type_of(item_value(p_element));
  }

  auto element_size(const value_type &p_element) -> llvm::Value * {
    if (!p_element.is_list && !p_element.pointee.empty() &&
        p_element.base != base_type::map)
      return builder.getInt64(module->getDataLayout().getTypeAllocSize(
          element_storage_type(p_element)));
    return builder.getInt64(is_byte(p_element) ? 1 : 8);
  }

  // Text written in the program lives in a section of its own, which `free`
  // recognises (runtime.cpp), so it is never freed by mistake.
  auto constant_text_global(const string_type &p_text) -> llvm::GlobalVariable * {
    auto *const global =
        builder.CreateGlobalString(p_text, "", 0, module.get());
#if defined(__APPLE__)
    global->setSection("__TEXT,__ovis_const");
#else
    global->setSection("ovis_const");
#endif
    return global;
  }

  auto constant_text(const string_type &p_text) -> llvm::Value * {
    return constant_text_global(p_text);
  }

  auto zero_of(const value_type &p_type) -> llvm::Value * {
    if (p_type == value_type::string())
      return constant_text("");
    if (p_type.is_list)
      return runtime_call("ovis_list_new", builder.getPtrTy(),
                          {element_size(item_storage(p_type)), builder.getInt64(0)});
    if (p_type.is_map())
      // The runtime's OVIS_KEY_NUMBER, OVIS_KEY_TEXT, OVIS_KEY_DECIMAL.
      return runtime_call("ovis_map_new", builder.getPtrTy(),
                          {builder.getInt64(p_type.map_key() == value_type::string() ? 1
                                            : p_type.map_key().is_decimal() ? 2
                                                                            : 0)});
    return llvm::Constant::getNullValue(type_of(p_type));
  }

  // A value where another type goes: a number into another number's type
  // (narrowing checked, numbers.md §3 rule 5).
  auto coerce(llvm::Value *p_value, const value_type &p_from,
              const value_type &p_to) -> llvm::Value * {
    if (p_from.is_numeric() && p_to.is_numeric())
      return convert_number(p_value, p_from, p_to, conversion::checked);
    return p_value;
  }

  // Numbers (docs/proposals/numbers.md).

  // How a number becomes another type that may not hold it: an error in
  // checked builds (truncated in fast ones), its low bits, or the nearest
  // value that fits.
  enum class conversion { checked, wrap, clamp };

  static auto is_unsigned_number(const value_type &p_type) -> bool {
    return p_type.is_whole() && core::is_unsigned(*p_type.number_type());
  }

  static auto bits_of(const value_type &p_type) -> unsigned {
    return core::number_bits(*p_type.number_type());
  }

  // A whole number made 128 bits wide, as its type says it is signed.
  auto widened(llvm::Value *p_value, const value_type &p_type) -> llvm::Value * {
    return is_unsigned_number(p_type)
               ? builder.CreateZExt(p_value, builder.getInt128Ty())
               : builder.CreateSExt(p_value, builder.getInt128Ty());
  }

  auto limits_of(const value_type &p_type)
      -> std::pair<llvm::Constant *, llvm::Constant *> {
    const auto bits = bits_of(p_type);
    if (is_unsigned_number(p_type))
      return {llvm::ConstantInt::get(builder.getInt128Ty(), 0),
              llvm::ConstantInt::get(
                  context, llvm::APInt::getMaxValue(bits).zext(128))};
    return {llvm::ConstantInt::get(
                context, llvm::APInt::getSignedMinValue(bits).sext(128)),
            llvm::ConstantInt::get(
                context, llvm::APInt::getSignedMaxValue(bits).sext(128))};
  }

  // Stops the program: the value doesn't fit in the type.
  auto narrow_failure(llvm::Value *p_condition, llvm::Value *p_value,
                      const value_type &p_from, const value_type &p_to)
      -> void {
    auto *const failure = llvm::BasicBlock::Create(
        context, "narrow.failure", builder.GetInsertBlock()->getParent());
    auto *const next = llvm::BasicBlock::Create(
        context, "narrow.ok", builder.GetInsertBlock()->getParent());
    builder.CreateCondBr(p_condition, failure, next);
    builder.SetInsertPoint(failure);
    auto *const shown =
        p_from.is_decimal()
            ? builder.CreateBitCast(
                  builder.CreateFPExt(p_value, builder.getDoubleTy()),
                  builder.getInt64Ty())
            : builder.CreateTrunc(widened(p_value, p_from), builder.getInt64Ty());
    runtime_call("ovis_narrow_failed", builder.getVoidTy(),
                 {shown,
                  builder.getInt32(p_from.is_decimal()      ? 2
                                   : is_unsigned_number(p_from) ? 1
                                                                : 0),
                  constant_text(core::describe_in_english(p_to) + " (" +
                                core::number_range(p_to) + ")")});
    builder.CreateUnreachable();
    builder.SetInsertPoint(next);
  }

  auto convert_number(llvm::Value *p_value, const value_type &p_from,
                      const value_type &p_to, conversion p_how)
      -> llvm::Value * {
    if (p_from == p_to)
      return p_value;
    auto *const target = type_of(p_to);
    if (p_from.is_whole() && p_to.is_decimal())
      return is_unsigned_number(p_from) ? builder.CreateUIToFP(p_value, target)
                                        : builder.CreateSIToFP(p_value, target);
    if (p_from.is_decimal() && p_to.is_decimal())
      return bits_of(p_from) < bits_of(p_to)
                 ? builder.CreateFPExt(p_value, target)
                 : builder.CreateFPTrunc(p_value, target);
    if (p_from.is_decimal()) {
      // Toward zero; out of range (or not a number) is an error, the
      // nearest value, or the low bits of the nearest 64-bit one.
      auto *const wide = builder.CreateFPExt(p_value, builder.getDoubleTy());
      const bool to_unsigned = is_unsigned_number(p_to);
      if (p_how == conversion::checked && checked) {
        const auto [low, high] = limits_of(p_to);
        auto *const low_real = llvm::ConstantFP::get(
            builder.getDoubleTy(),
            static_cast<double>(low->getUniqueInteger().getSExtValue()) - 1.0);
        auto *const high_real = llvm::ConstantFP::get(
            builder.getDoubleTy(),
            to_unsigned ? static_cast<double>(high->getUniqueInteger().getZExtValue()) + 1.0
                        : static_cast<double>(high->getUniqueInteger().getSExtValue()) + 1.0);
        auto *const inside = builder.CreateAnd(builder.CreateFCmpOGT(wide, low_real),
                                               builder.CreateFCmpOLT(wide, high_real));
        narrow_failure(builder.CreateNot(inside), p_value, p_from, p_to);
      }
      if (p_how == conversion::wrap) {
        auto *const whole = saturating_to_whole(wide, builder.getInt64Ty(), false);
        return builder.CreateTrunc(whole, target);
      }
      return saturating_to_whole(wide, target, to_unsigned);
    }
    // Whole to whole.
    const auto from_bits = bits_of(p_from);
    const auto to_bits = bits_of(p_to);
    const bool from_unsigned = is_unsigned_number(p_from);
    const bool to_unsigned = is_unsigned_number(p_to);
    const auto wrapped = [&] {
      if (to_bits < from_bits)
        return builder.CreateTrunc(p_value, target);
      if (to_bits > from_bits)
        return from_unsigned ? builder.CreateZExt(p_value, target)
                             : builder.CreateSExt(p_value, target);
      return p_value;
    };
    const bool always_fits =
        (to_bits > from_bits && (from_unsigned || !to_unsigned)) ||
        (to_bits == from_bits && from_unsigned == to_unsigned);
    if (always_fits || p_how == conversion::wrap ||
        (p_how == conversion::checked && !checked))
      return wrapped();
    const auto [low, high] = limits_of(p_to);
    auto *const wide = widened(p_value, p_from);
    auto *const below = builder.CreateICmpSLT(wide, low);
    auto *const above = builder.CreateICmpSGT(wide, high);
    if (p_how == conversion::checked) {
      narrow_failure(builder.CreateOr(below, above), p_value, p_from, p_to);
      return wrapped();
    }
    return builder.CreateTrunc(
        builder.CreateSelect(below, low, builder.CreateSelect(above, high, wide)),
        target);
  }

  // A decimal's whole number toward zero, the nearest one that fits (not a
  // number is 0): LLVM's saturating conversions.
  auto saturating_to_whole(llvm::Value *p_value, llvm::Type *p_type,
                           bool p_unsigned) -> llvm::Value * {
#if LLVM_VERSION_MAJOR >= 20
    auto *const declaration = llvm::Intrinsic::getOrInsertDeclaration(
#else
    auto *const declaration = llvm::Intrinsic::getDeclaration(
#endif
        module.get(),
        p_unsigned ? llvm::Intrinsic::fptoui_sat : llvm::Intrinsic::fptosi_sat,
        {p_type, p_value->getType()});
    return builder.CreateCall(declaration, {p_value});
  }

  // A number written in the program, as the type the checker gave it.
  auto number_constant(const expression &p_number) -> llvm::Value * {
    const auto &type = p_number.type;
    if (p_number.kind == expression::kind_type::real ||
        (type.is_decimal())) {
      const double value =
          p_number.kind == expression::kind_type::real ? p_number.real
          : p_number.is_big
              ? static_cast<double>(static_cast<max_uint_type>(p_number.integer))
              : static_cast<double>(p_number.integer);
      return llvm::ConstantFP::get(type.is_decimal() ? type_of(type)
                                                     : builder.getDoubleTy(),
                                   value);
    }
    return llvm::ConstantInt::get(
        type.is_whole() ? type_of(type) : builder.getInt64Ty(),
        static_cast<std::uint64_t>(p_number.integer), !p_number.is_big);
  }

  // Runtime and intrinsics.

  auto runtime_call(const char *p_name, llvm::Type *p_return,
                    std::initializer_list<llvm::Value *> p_arguments)
      -> llvm::Value * {
    std::vector<llvm::Type *> parameters;
    for (auto *const argument : p_arguments)
      parameters.push_back(argument->getType());
    const auto callee = module->getOrInsertFunction(
        p_name, llvm::FunctionType::get(p_return, parameters, false));
    return builder.CreateCall(callee, p_arguments);
  }

  auto intrinsic(llvm::Intrinsic::ID p_id,
                 std::initializer_list<llvm::Value *> p_arguments)
      -> llvm::Value * {
    // LLVM 20 renamed getDeclaration; keep building against LLVM 18 too.
#if LLVM_VERSION_MAJOR >= 20
    auto *const declaration = llvm::Intrinsic::getOrInsertDeclaration(
        module.get(), p_id, {builder.getDoubleTy()});
#else
    auto *const declaration = llvm::Intrinsic::getDeclaration(
        module.get(), p_id, {builder.getDoubleTy()});
#endif
    return builder.CreateCall(declaration, p_arguments);
  }

  // Variables.

  auto create_entry_alloca(llvm::Type *p_type, const string_type &p_name)
      -> llvm::AllocaInst * {
    auto &entry = current_function->getEntryBlock();
    llvm::IRBuilder<> entry_builder(&entry, entry.begin());
    return entry_builder.CreateAlloca(p_type, nullptr, p_name);
  }

  auto find_variable(const string_type &p_name) -> variable * {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
      if (const auto found = scope->find(p_name); found != scope->end())
        return &found->second;
    if (const auto found = globals.find(p_name); found != globals.end())
      return &found->second;
    return nullptr;
  }

  auto lookup(const string_type &p_name) -> variable & {
    return *find_variable(p_name);
  }

  auto generate_constant(const core::c_constant &p_constant)
      -> llvm::Value * {
    using kind = core::c_constant::kind_type;
    switch (p_constant.kind) {
    case kind::integer:
      return builder.getInt64(p_constant.integer);
    case kind::real:
      return llvm::ConstantFP::get(builder.getDoubleTy(), p_constant.real);
    case kind::null:
      return llvm::ConstantPointerNull::get(builder.getPtrTy());
    case kind::runtime:
      return runtime_call(p_constant.getter.c_str(),
                          p_constant.name.starts_with("std")
                              ? static_cast<llvm::Type *>(builder.getPtrTy())
                              : builder.getInt64Ty(),
                          {});
    }
    return nullptr;
  }

  auto declare_local(const string_type &p_name, const value_type &p_type)
      -> variable & {
    auto *const address = create_entry_alloca(type_of(p_type), p_name);
    return scopes.back()[p_name] = variable{address, p_type};
  }

  // Whole-number `+`, `-` and `*` that stop the program when the result
  // doesn't fit its type, instead of wrapping around.
  auto checked_integer(llvm::Intrinsic::ID p_id, llvm::Value *p_left,
                       llvm::Value *p_right, const value_type &p_type)
      -> llvm::Value * {
    if (arithmetic != arithmetic_checked)
      return unchecked_integer(p_id, p_left, p_right, p_type);
#if LLVM_VERSION_MAJOR >= 20
    auto *const declaration = llvm::Intrinsic::getOrInsertDeclaration(
        module.get(), p_id, {p_left->getType()});
#else
    auto *const declaration = llvm::Intrinsic::getDeclaration(
        module.get(), p_id, {p_left->getType()});
#endif
    auto *const pair = builder.CreateCall(declaration, {p_left, p_right});
    auto *const overflowed = builder.CreateExtractValue(pair, 1);
    auto *const failure = llvm::BasicBlock::Create(
        context, "overflow", builder.GetInsertBlock()->getParent());
    auto *const next = llvm::BasicBlock::Create(
        context, "no.overflow", builder.GetInsertBlock()->getParent());
    builder.CreateCondBr(overflowed, failure, next);
    builder.SetInsertPoint(failure);
    runtime_call("ovis_overflow", builder.getVoidTy(), {overflow_text(p_type)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(next);
    return builder.CreateExtractValue(pair, 0);
  }

  // `+`, `-`, `*` inside `(wrapping …)` (the low bits) or `(saturating …)`
  // (the nearest value that fits).
  auto unchecked_integer(llvm::Intrinsic::ID p_id, llvm::Value *p_left,
                         llvm::Value *p_right, const value_type &p_type)
      -> llvm::Value * {
    using llvm::Intrinsic::ID;
    const bool is_add = p_id == llvm::Intrinsic::sadd_with_overflow ||
                        p_id == llvm::Intrinsic::uadd_with_overflow;
    const bool is_subtract = p_id == llvm::Intrinsic::ssub_with_overflow ||
                             p_id == llvm::Intrinsic::usub_with_overflow;
    if (arithmetic == arithmetic_wrapping)
      return is_add        ? builder.CreateAdd(p_left, p_right)
             : is_subtract ? builder.CreateSub(p_left, p_right)
                           : builder.CreateMul(p_left, p_right);
    const bool is_unsigned = is_unsigned_number(p_type);
    if (is_add || is_subtract)
      return integer_intrinsic(
          is_add ? (is_unsigned ? llvm::Intrinsic::uadd_sat : llvm::Intrinsic::sadd_sat)
                 : (is_unsigned ? llvm::Intrinsic::usub_sat : llvm::Intrinsic::ssub_sat),
          {p_left, p_right});
    // A product is worked out in 128 bits, then clamped.
    const auto [low, high] = limits_of(p_type);
    auto *const product =
        builder.CreateMul(widened(p_left, p_type), widened(p_right, p_type));
    return builder.CreateTrunc(
        builder.CreateSelect(
            builder.CreateICmpSLT(product, low), low,
            builder.CreateSelect(builder.CreateICmpSGT(product, high), high,
                                 product)),
        p_left->getType());
  }

  // A number's bytes, lowest first or highest first, as a new list of
  // bytes; and a number read from a list of bytes at an index.
  auto generate_bytes(const expression &p_call) -> llvm::Value * {
    const auto &name = p_call.text;
    const auto &arguments = p_call.arguments;
    const bool big = name.ends_with("-big");
    const auto byte = value_type::number(core::c_type::uint8_type);
    if (name.starts_with("to-bytes")) {
      const auto &type = arguments[0].type;
      if (type == value_type::string())
        return runtime_call("ovis_text_bytes", builder.getPtrTy(),
                            {generate(arguments[0])});
      auto *value = generate(arguments[0]);
      const auto bits = bits_of(type);
      auto *const whole = builder.getIntNTy(bits);
      value = builder.CreateBitCast(value, whole);
      if (big && bits > 8)
        value = integer_intrinsic(llvm::Intrinsic::bswap, {value});
      auto *const list = runtime_call("ovis_list_new", builder.getPtrTy(),
                                      {builder.getInt64(1), builder.getInt64(bits / 8)});
      builder.CreateAlignedStore(
          value, runtime_call("ovis_list_data", builder.getPtrTy(), {list}),
          llvm::MaybeAlign(1));
      return list;
    }
    const auto &type = p_call.type;
    const auto bits = bits_of(type);
    auto *const list = generate(arguments[0]);
    auto *const index = coerce(generate(arguments[1]), arguments[1].type,
                               value_type::integer());
    // Its first and last bytes must be in the list.
    element_address(list, builder.CreateAdd(index, builder.getInt64(bits / 8 - 1)),
                    byte);
    auto *const first = element_address(list, index, byte);
    auto *value = static_cast<llvm::Value *>(builder.CreateAlignedLoad(
        builder.getIntNTy(bits), first, llvm::MaybeAlign(1)));
    if (big && bits > 8)
      value = integer_intrinsic(llvm::Intrinsic::bswap, {value});
    return builder.CreateBitCast(value, type_of(type));
  }

  // What got too big, for the runtime error: "an int16 (-32768 to 32767)".
  auto overflow_text(const value_type &p_type) -> llvm::Value * {
    return constant_text(core::describe_in_english(p_type) +
                         (p_type.is_sized() ? " (" + core::number_range(p_type) + ")"
                                            : string_type()));
  }

  // An intrinsic over one whole number type (`ctpop`, `fshl`, `bswap`).
  auto integer_intrinsic(llvm::Intrinsic::ID p_id,
                         std::initializer_list<llvm::Value *> p_arguments)
      -> llvm::Value * {
#if LLVM_VERSION_MAJOR >= 20
    auto *const declaration = llvm::Intrinsic::getOrInsertDeclaration(
        module.get(), p_id, {(*p_arguments.begin())->getType()});
#else
    auto *const declaration = llvm::Intrinsic::getDeclaration(
        module.get(), p_id, {(*p_arguments.begin())->getType()});
#endif
    return builder.CreateCall(declaration, p_arguments);
  }

  // A divisor of zero stops the program, and so does the one division
  // whose result doesn't fit: the smallest signed number by -1.
  auto check_division(llvm::Value *p_left, llvm::Value *p_right,
                      const value_type &p_type) -> void {
    runtime_call("ovis_check_divisor", builder.getVoidTy(),
                 {builder.CreateTrunc(widened(p_right, p_type),
                                      builder.getInt64Ty())});
    if (is_unsigned_number(p_type))
      return;
    const auto bits = bits_of(p_type);
    auto *const too_big = builder.CreateAnd(
        builder.CreateICmpEQ(p_right, llvm::ConstantInt::getSigned(p_right->getType(), -1)),
        builder.CreateICmpEQ(
            p_left, llvm::ConstantInt::get(context, llvm::APInt::getSignedMinValue(bits))));
    auto *const failure = llvm::BasicBlock::Create(
        context, "overflow", builder.GetInsertBlock()->getParent());
    auto *const next = llvm::BasicBlock::Create(
        context, "no.overflow", builder.GetInsertBlock()->getParent());
    builder.CreateCondBr(too_big, failure, next);
    builder.SetInsertPoint(failure);
    runtime_call("ovis_overflow", builder.getVoidTy(), {overflow_text(p_type)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(next);
  }

  // A bit's place, as the number's own type: one outside its bits is an
  // error in checked builds (LLVM would give poison), masked in fast ones.
  auto bit_place(const expression &p_place, llvm::Type *p_type, unsigned p_bits)
      -> llvm::Value * {
    auto *place =
        coerce(generate(p_place), p_place.type, value_type::integer());
    if (checked)
      runtime_call("ovis_check_bit_place", builder.getVoidTy(),
                   {place, builder.getInt64(p_bits)});
    else
      place = builder.CreateAnd(place, builder.getInt64(p_bits - 1));
    return builder.CreateTrunc(place, p_type);
  }

  // A whole number's bits (numbers.md §4).
  auto generate_bits(const expression &p_call) -> llvm::Value * {
    const auto &name = p_call.text;
    const auto &arguments = p_call.arguments;
    const bool is_joined =
        name == "bit-and" || name == "bit-or" || name == "bit-xor";
    const auto type = is_joined ? p_call.type : arguments[0].type;
    auto *value = coerce(generate(arguments[0]), arguments[0].type, type);
    auto *const llvm_type = value->getType();
    const auto bits = bits_of(type);
    if (is_joined) {
      for (std::size_t index = 1; index < arguments.size(); ++index) {
        auto *const right =
            coerce(generate(arguments[index]), arguments[index].type, type);
        value = name == "bit-and"  ? builder.CreateAnd(value, right)
                : name == "bit-or" ? builder.CreateOr(value, right)
                                   : builder.CreateXor(value, right);
      }
      return value;
    }
    const auto count = [&](llvm::Value *p_count) {
      return builder.CreateZExtOrTrunc(p_count, builder.getInt64Ty());
    };
    if (name == "bit-not")
      return builder.CreateNot(value);
    if (name == "count-ones")
      return count(integer_intrinsic(llvm::Intrinsic::ctpop, {value}));
    if (name == "leading-zeros")
      return count(integer_intrinsic(llvm::Intrinsic::ctlz,
                                     {value, builder.getInt1(false)}));
    if (name == "trailing-zeros")
      return count(integer_intrinsic(llvm::Intrinsic::cttz,
                                     {value, builder.getInt1(false)}));
    if (name == "byte-swap")
      return bits == 8 ? value : integer_intrinsic(llvm::Intrinsic::bswap, {value});
    if (name == "bit-field") {
      // Bits LOW to HIGH, moved down to the lowest.
      auto *const low = coerce(generate(arguments[1]), arguments[1].type,
                               value_type::integer());
      auto *const high = coerce(generate(arguments[2]), arguments[2].type,
                                value_type::integer());
      runtime_call("ovis_check_bit_range", builder.getVoidTy(),
                   {low, high, builder.getInt64(bits)});
      auto *const shifted =
          builder.CreateLShr(value, builder.CreateTrunc(low, llvm_type));
      auto *const mask = builder.CreateLShr(
          llvm::ConstantInt::getAllOnesValue(llvm_type),
          builder.CreateTrunc(
              builder.CreateSub(builder.getInt64(bits - 1),
                                builder.CreateSub(high, low)),
              llvm_type));
      return builder.CreateAnd(shifted, mask);
    }
    auto *const place = bit_place(arguments[1], llvm_type, bits);
    if (name == "shift-left")
      return builder.CreateShl(value, place);
    if (name == "shift-right")
      return is_unsigned_number(type) ? builder.CreateLShr(value, place)
                                      : builder.CreateAShr(value, place);
    if (name == "rotate-left")
      return integer_intrinsic(llvm::Intrinsic::fshl, {value, value, place});
    if (name == "rotate-right")
      return integer_intrinsic(llvm::Intrinsic::fshr, {value, value, place});
    if (name == "bit-test")
      return builder.CreateTrunc(builder.CreateLShr(value, place),
                                 builder.getInt1Ty());
    auto *const one = builder.CreateShl(llvm::ConstantInt::get(llvm_type, 1), place);
    if (name == "bit-set")
      return builder.CreateOr(value, one);
    if (name == "bit-clear")
      return builder.CreateAnd(value, builder.CreateNot(one));
    return builder.CreateXor(value, one); // bit-flip
  }

  // `(to-hex X [DIGITS])`, `(to-binary X [DIGITS])`: a number of a size
  // shows all its bits (a byte two hex digits), a whole number only those
  // it needs; DIGITS is the fewest.
  auto generate_digits(const expression &p_call) -> llvm::Value * {
    const auto &arguments = p_call.arguments;
    const auto &type = arguments[0].type;
    auto *const value = builder.CreateZExt(generate(arguments[0]),
                                           builder.getInt64Ty());
    const bool is_hex = p_call.text == "to-hex";
    const auto bits = bits_of(type);
    auto *const digits =
        arguments.size() == 2
            ? coerce(generate(arguments[1]), arguments[1].type,
                     value_type::integer())
            : builder.getInt64(type.is_sized() ? (is_hex ? bits / 4 : bits) : 1);
    return runtime_call("ovis_digits", builder.getPtrTy(),
                        {value, digits, builder.getInt64(is_hex ? 4 : 1)});
  }

  // Expressions.

  auto generate_arithmetic(const expression &p_call) -> llvm::Value * {
    const auto &name = p_call.text;
    // `/` of whole numbers is a decimal, worked out as decimals.
    const auto type = p_call.type;
    const bool is_real = type.is_decimal();
    const bool is_unsigned = is_unsigned_number(type);
    using intrinsic_id = llvm::Intrinsic::ID;
    const intrinsic_id add = is_unsigned ? llvm::Intrinsic::uadd_with_overflow
                                         : llvm::Intrinsic::sadd_with_overflow;
    const intrinsic_id subtract = is_unsigned
                                      ? llvm::Intrinsic::usub_with_overflow
                                      : llvm::Intrinsic::ssub_with_overflow;
    const intrinsic_id multiply = is_unsigned
                                      ? llvm::Intrinsic::umul_with_overflow
                                      : llvm::Intrinsic::smul_with_overflow;

    auto operand = [&](std::size_t p_index) {
      const auto &argument = p_call.arguments[p_index];
      return coerce(generate(argument), argument.type, type);
    };

    if (name == "-" && p_call.arguments.size() == 1)
      return is_real ? builder.CreateFNeg(operand(0))
                     : checked_integer(subtract,
                                       llvm::ConstantInt::get(type_of(type), 0),
                                       operand(0), type);

    auto *result = operand(0);
    for (std::size_t index = 1; index < p_call.arguments.size(); ++index) {
      auto *const right = operand(index);
      if (name == "+") {
        result = is_real ? builder.CreateFAdd(result, right)
                         : checked_integer(add, result, right, type);
      } else if (name == "-") {
        result = is_real ? builder.CreateFSub(result, right)
                         : checked_integer(subtract, result, right, type);
      } else if (name == "*") {
        result = is_real ? builder.CreateFMul(result, right)
                         : checked_integer(multiply, result, right, type);
      } else if (name == "/" || name == "%" || name == "div") {
        if (is_real) {
          result = name == "%" ? builder.CreateFRem(result, right)
                               : builder.CreateFDiv(result, right);
        } else {
          check_division(result, right, type);
          result = name == "%"
                       ? (is_unsigned ? builder.CreateURem(result, right)
                                      : builder.CreateSRem(result, right))
                       : (is_unsigned ? builder.CreateUDiv(result, right)
                                      : builder.CreateSDiv(result, right));
        }
      } else {
        // min / max
        auto *const right_wins =
            is_real ? (name == "min" ? builder.CreateFCmpOLT(right, result)
                                     : builder.CreateFCmpOGT(right, result))
            : is_unsigned ? (name == "min" ? builder.CreateICmpULT(right, result)
                                           : builder.CreateICmpUGT(right, result))
                          : (name == "min" ? builder.CreateICmpSLT(right, result)
                                           : builder.CreateICmpSGT(right, result));
        result = builder.CreateSelect(right_wins, right, result);
      }
    }
    return result;
  }

  auto generate_comparison(const expression &p_call) -> llvm::Value * {
    const auto &left_expression = p_call.arguments[0];
    const auto &right_expression = p_call.arguments[1];
    const auto &name = p_call.text;
    auto *left = generate(left_expression);
    auto *right = generate(right_expression);

    if (left_expression.type.is_pointer() ||
        right_expression.type.is_pointer())
      return name == "=" ? builder.CreateICmpEQ(left, right)
                         : builder.CreateICmpNE(left, right);
    if (left_expression.type == value_type::string()) {
      auto *const equal = builder.CreateICmpNE(
          runtime_call("ovis_str_eq", builder.getInt32Ty(), {left, right}),
          builder.getInt32(0));
      if (name == "=" || name == "!=")
        return name == "=" ? equal : builder.CreateNot(equal);
      // Ordered text compares the sign of strcmp with 0.
      left = runtime_call("ovis_str_cmp", builder.getInt32Ty(), {left, right});
      right = builder.getInt32(0);
    }

    bool is_unsigned = false;
    if (left_expression.type.is_numeric()) {
      const auto common =
          core::common_number(left_expression.type, right_expression.type)
              .value_or(value_type::integer());
      left = coerce(left, left_expression.type, common);
      right = coerce(right, right_expression.type, common);
      is_unsigned = is_unsigned_number(common);
      if (common.is_decimal()) {
        if (name == "=")
          return builder.CreateFCmpOEQ(left, right);
        if (name == "!=")
          return builder.CreateFCmpUNE(left, right);
        if (name == "<")
          return builder.CreateFCmpOLT(left, right);
        if (name == "<=")
          return builder.CreateFCmpOLE(left, right);
        if (name == ">")
          return builder.CreateFCmpOGT(left, right);
        return builder.CreateFCmpOGE(left, right);
      }
    }
    if (name == "=")
      return builder.CreateICmpEQ(left, right);
    if (name == "!=")
      return builder.CreateICmpNE(left, right);
    if (name == "<")
      return is_unsigned ? builder.CreateICmpULT(left, right)
                         : builder.CreateICmpSLT(left, right);
    if (name == "<=")
      return is_unsigned ? builder.CreateICmpULE(left, right)
                         : builder.CreateICmpSLE(left, right);
    if (name == ">")
      return is_unsigned ? builder.CreateICmpUGT(left, right)
                         : builder.CreateICmpSGT(left, right);
    return is_unsigned ? builder.CreateICmpUGE(left, right)
                       : builder.CreateICmpSGE(left, right);
  }

  // `and` / `or` only evaluate as many operands as needed.
  auto generate_logical(const expression &p_call) -> llvm::Value * {
    const bool is_and = p_call.text == "and";
    auto *const end_block = llvm::BasicBlock::Create(context, "logic.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;

    for (std::size_t index = 0; index < p_call.arguments.size(); ++index) {
      auto *const value = generate(p_call.arguments[index]);
      if (index + 1 == p_call.arguments.size()) {
        incoming.emplace_back(value, builder.GetInsertBlock());
        builder.CreateBr(end_block);
        break;
      }
      auto *const next_block = llvm::BasicBlock::Create(
          context, "logic.next", current_function);
      incoming.emplace_back(builder.getInt1(!is_and), builder.GetInsertBlock());
      if (is_and)
        builder.CreateCondBr(value, next_block, end_block);
      else
        builder.CreateCondBr(value, end_block, next_block);
      builder.SetInsertPoint(next_block);
    }

    end_block->insertInto(current_function);
    builder.SetInsertPoint(end_block);
    auto *const phi = builder.CreatePHI(builder.getInt1Ty(), incoming.size());
    for (const auto &[value, block] : incoming)
      phi->addIncoming(value, block);
    return phi;
  }

  auto element_address(llvm::Value *p_list, llvm::Value *p_index,
                       const value_type &p_element) -> llvm::Value * {
    return runtime_call("ovis_list_at", builder.getPtrTy(),
                        {p_list, p_index, element_size(p_element)});
  }

  auto store_element(llvm::Value *p_address, llvm::Value *p_value,
                     const value_type &p_element) -> void {
    if (is_byte(p_element))
      p_value = builder.CreateZExt(p_value, builder.getInt8Ty());
    if (!p_element.is_list && !p_element.pointee.empty() &&
        p_element.base != base_type::map)
      p_value = to_c(p_value, item_value(p_element),
                     *core::c_number_type(p_element.pointee));
    builder.CreateStore(p_value, p_address);
  }

  auto load_element(llvm::Value *p_address, const value_type &p_element)
      -> llvm::Value * {
    auto *const value =
        builder.CreateLoad(element_storage_type(p_element), p_address);
    if (is_byte(p_element))
      return builder.CreateICmpNE(value, builder.getInt8(0));
    return value;
  }

  auto to_text(llvm::Value *p_value, const value_type &p_type)
      -> llvm::Value * {
    switch (p_type.base) {
    case base_type::integer:
      if (*p_type.number_type() == core::c_type::size_type)
        return runtime_call("ovis_uint_to_str", builder.getPtrTy(), {p_value});
      return runtime_call("ovis_int_to_str", builder.getPtrTy(),
                          {coerce(p_value, p_type, value_type::integer())});
    case base_type::real:
      if (p_type.is_sized())
        return runtime_call("ovis_float32_to_str", builder.getPtrTy(), {p_value});
      return runtime_call("ovis_float_to_str", builder.getPtrTy(), {p_value});
    case base_type::boolean:
      return runtime_call("ovis_bool_to_str", builder.getPtrTy(),
                          {builder.CreateZExt(p_value, builder.getInt32Ty())});
    default:
      return p_value;
    }
  }

  // Records.

  auto field_index(const string_type &p_record, const string_type &p_field)
      -> unsigned {
    const auto &fields = records.at(p_record)->fields;
    for (unsigned index = 0; index < fields.size(); ++index)
      if (fields[index].name == p_field)
        return index + (records.at(p_record)->has_tag() ? 1 : 0);
    throw error("Internal compiler error: no field " + p_field + ".");
  }

  // The address of a field, after making sure the record exists: a variable
  // declared before it is given a record holds null. A C record's fields
  // are at the offsets clang gave.
  auto field_address(llvm::Value *p_record, const string_type &p_name,
                     const string_type &p_field) -> llvm::Value * {
    runtime_call("ovis_record_check", builder.getVoidTy(), {p_record});
    const auto &declaration = *records.at(p_name);
    if (declaration.is_c)
      return builder.CreateInBoundsGEP(
          builder.getInt8Ty(), p_record,
          builder.getInt64(declaration.find(p_field)->offset));
    return builder.CreateStructGEP(record_types.at(p_name), p_record,
                                   field_index(p_name, p_field));
  }

  // A field's value: a C record's converted from how C keeps it; its text
  // (`char[N]`) and structs held in it are where they are, valid while
  // the record is.
  auto load_field(llvm::Value *p_record, const string_type &p_name,
                  const core::record_field &p_field) -> llvm::Value * {
    auto *const address = field_address(p_record, p_name, p_field.name);
    if (p_field.text_length > 0 || p_field.inline_record)
      return address;
    if (p_field.storage)
      return from_c(builder.CreateLoad(c_llvm_type(builder, *p_field.storage),
                                       address),
                    *p_field.storage);
    return builder.CreateLoad(type_of(p_field.type), address);
  }

  auto store_field(llvm::Value *p_record, const string_type &p_name,
                   const core::record_field &p_field, llvm::Value *p_value,
                   const value_type &p_from) -> void {
    auto *const address = field_address(p_record, p_name, p_field.name);
    if (p_field.text_length > 0) {
      runtime_call("ovis_c_text_set", builder.getVoidTy(),
                   {address, builder.getInt64(p_field.text_length), p_value});
      return;
    }
    if (p_field.inline_record) {
      builder.CreateMemCpy(address, llvm::MaybeAlign(1), p_value,
                           llvm::MaybeAlign(1),
                           records.at(p_field.type.record)->size);
      return;
    }
    // A byte array into a C `char *` field: its items.
    if (p_from.is_list)
      p_value = runtime_call("ovis_list_data", builder.getPtrTy(), {p_value});
    builder.CreateStore(p_field.storage
                            ? to_c(p_value, p_from, *p_field.storage)
                            : coerce(p_value, p_from, p_field.type),
                        address);
  }

  auto generate_new(const expression &p_call) -> llvm::Value * {
    const auto &name = p_call.type_argument->record;
    auto *const type = record_types.at(name);
    if (records.at(name)->is_c) {
      // Zeroed, as C's `= {0}`; no tag.
      auto *const record = runtime_call(
          "ovis_record_new", builder.getPtrTy(),
          {builder.getInt64(std::max<std::uint64_t>(records.at(name)->size, 1))});
      for (std::size_t index = 0; index < p_call.arguments.size(); ++index)
        store_field(record, name, *records.at(name)->find(p_call.fields[index]),
                    generate(p_call.arguments[index]),
                    p_call.arguments[index].type);
      return record;
    }
    auto *const record =
        runtime_call("ovis_record_new", builder.getPtrTy(),
                     {llvm::ConstantExpr::getSizeOf(type)});
    if (records.at(name)->has_tag())
      builder.CreateStore(builder.getInt64(record_tags.at(name)),
                          builder.CreateStructGEP(type, record, 0));
    for (std::size_t index = 0; index < p_call.arguments.size(); ++index) {
      const auto &field_name = p_call.fields[index];
      const auto &field = *records.at(name)->find(field_name);
      builder.CreateStore(coerce(generate(p_call.arguments[index]),
                                 p_call.arguments[index].type, field.type),
                          builder.CreateStructGEP(
                              type, record, field_index(name, field_name)));
    }
    return record;
  }

  // A method called on a value known only as a trait: the function is found
  // in the method's table by the tag at the start of the record.
  auto generate_dispatch(const expression &p_call) -> llvm::Value * {
    const auto &signature = *method_owner.at(p_call.text)->find(p_call.text);
    auto *const self = generate(p_call.arguments[0]);
    runtime_call("ovis_record_check", builder.getVoidTy(), {self});
    llvm::Value *callee = nullptr;
    // Only one record answers it: call its function directly (LLVM may
    // inline it), once the value is checked to be that record. A value of
    // another trait may be any record that implements both.
    if (const auto only = single_implementation.find(p_call.text);
        only != single_implementation.end()) {
      auto *const tag = builder.CreateLoad(builder.getInt64Ty(), self);
      callee = only->second.second;
      runtime_call("ovis_method_check", builder.getVoidTy(),
                   {builder.CreateSelect(
                       builder.CreateICmpEQ(tag, builder.getInt64(only->second.first)),
                       callee, llvm::Constant::getNullValue(builder.getPtrTy()))});
    } else {
      auto *const tag = builder.CreateLoad(builder.getInt64Ty(), self);
      auto *const table = dispatch_tables.at(p_call.text);
      auto *const entry = builder.CreateInBoundsGEP(
          table->getValueType(), table, {builder.getInt64(0), tag});
      callee = builder.CreateLoad(builder.getPtrTy(), entry);
      runtime_call("ovis_method_check", builder.getVoidTy(), {callee});
    }
    std::vector<llvm::Type *> parameters;
    std::vector<llvm::Value *> values{self};
    for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
      parameters.push_back(type_of(signature.parameters[index].type));
      if (index > 0)
        values.push_back(coerce(generate(p_call.arguments[index]),
                                p_call.arguments[index].type,
                                signature.parameters[index].type));
      // `(a) comes before (b)`: b must be the record a's impl takes.
      if (index > 0 && signature.is_same(index))
        runtime_call("ovis_same_record_check", builder.getVoidTy(),
                     {self, values.back()});
    }
    return builder.CreateCall(
        llvm::FunctionType::get(type_of(signature.return_type), parameters,
                                false),
        callee, values);
  }

  // The parts of a join, nested joins flattened: `a + (b + c)` is three.
  static auto join_parts(const expression &p_part,
                         std::vector<const expression *> &p_parts) -> void {
    if (p_part.kind == expression::kind_type::call &&
        ((p_part.text == "+" && p_part.type == value_type::string()) ||
         p_part.text == "concat"))
      for (const auto &argument : p_part.arguments)
        join_parts(argument, p_parts);
    // `(row * column as text)`: a number or truth value written into the
    // join, not made into text of its own first.
    else if (p_part.kind == expression::kind_type::call &&
             p_part.text == "to_str" && p_part.arguments.size() == 1 &&
             (p_part.arguments[0].type == value_type::integer() ||
              p_part.arguments[0].type == value_type::real() ||
              p_part.arguments[0].type == value_type::boolean()))
      p_parts.push_back(&p_part.arguments[0]);
    else
      p_parts.push_back(&p_part);
  }

  // One join is one text: every part is written into one allocation
  // (proposals/memory.md §2). Numbers are formatted in small buffers.
  auto generate_concat(const expression &p_call) -> llvm::Value * {
    std::vector<const expression *> parts;
    join_parts(p_call, parts);
    if (parts.size() == 1)
      return to_text(generate(*parts.front()), parts.front()->type);
    auto *const array = create_entry_alloca(
        llvm::ArrayType::get(builder.getPtrTy(), parts.size()), "join.parts");
    for (std::size_t index = 0; index < parts.size(); ++index) {
      const auto &part = *parts[index];
      auto *const value = generate(part);
      llvm::Value *text = value;
      switch (part.type.base) {
      case base_type::integer:
      case base_type::real: {
        auto *const buffer = create_entry_alloca(
            llvm::ArrayType::get(builder.getInt8Ty(), 32), "join.number");
        const auto number = *part.type.number_type();
        if (part.type.is_decimal())
          runtime_call(number == core::c_type::float_type ? "ovis_format_float32"
                                                          : "ovis_format_float",
                       builder.getVoidTy(), {buffer, value});
        else if (number == core::c_type::size_type)
          runtime_call("ovis_format_uint", builder.getVoidTy(), {buffer, value});
        else
          runtime_call("ovis_format_int", builder.getVoidTy(),
                       {buffer, coerce(value, part.type, value_type::integer())});
        text = buffer;
        break;
      }
      case base_type::boolean:
        text = builder.CreateSelect(value, constant_text("true"),
                                    constant_text("false"));
        break;
      default:
        break;
      }
      builder.CreateStore(text,
                          builder.CreateConstInBoundsGEP2_32(array->getAllocatedType(),
                                                            array, 0, index));
    }
    return runtime_call("ovis_join", builder.getPtrTy(),
                        {builder.getInt64(parts.size()), array});
  }

  static auto c_llvm_type(llvm::IRBuilder<> &p_builder, core::c_type p_type)
      -> llvm::Type * {
    using enum core::c_type;
    switch (p_type) {
    case void_type:
      return p_builder.getVoidTy();
    case int8_type:
    case uint8_type:
      return p_builder.getInt8Ty();
    case int16_type:
    case uint16_type:
      return p_builder.getInt16Ty();
    case int_type:
    case uint_type:
      return p_builder.getInt32Ty();
    case long_type:
    case size_type:
      return p_builder.getInt64Ty();
    case double_type:
      return p_builder.getDoubleTy();
    case float_type:
      return p_builder.getFloatTy();
    case string_type:
    case pointer_type:
      return p_builder.getPtrTy();
    }
    return nullptr;
  }

  // Converts an Ovis value to a C type.
  auto to_c(llvm::Value *p_value, const value_type &p_from, core::c_type p_to)
      -> llvm::Value * {
    // A number into a C number: narrowed as into any number of that size
    // (checked; numbers.md §3 rule 5).
    if (p_from.is_numeric() && core::ovis_type_of(p_to).is_numeric())
      return convert_number(p_value, p_from, value_type::number(p_to),
                            conversion::checked);
    p_value = coerce(p_value, p_from, core::ovis_type_of(p_to));
    auto *const type = c_llvm_type(builder, p_to);
    if (p_to == core::c_type::float_type)
      return builder.CreateFPTrunc(p_value, type);
    if (!type->isIntegerTy() || type->getIntegerBitWidth() == 64)
      return p_value;
    return p_from == value_type::boolean()
               ? builder.CreateZExt(p_value, type)
               : builder.CreateTrunc(p_value, type);
  }

  // Converts a C value to its Ovis type.
  auto from_c(llvm::Value *p_value, core::c_type p_type) -> llvm::Value * {
    if (p_type == core::c_type::float_type)
      return builder.CreateFPExt(p_value, builder.getDoubleTy());
    if (!p_value->getType()->isIntegerTy() ||
        p_value->getType()->getIntegerBitWidth() == 64)
      return p_value;
    return core::is_unsigned(p_type)
               ? builder.CreateZExt(p_value, builder.getInt64Ty())
               : builder.CreateSExt(p_value, builder.getInt64Ty());
  }

  // A variadic argument with C's default promotions.
  auto to_c_variadic(llvm::Value *p_value, const value_type &p_type)
      -> llvm::Value * {
    if (p_type == value_type::boolean())
      return builder.CreateZExt(p_value, builder.getInt32Ty());
    // C's default promotions: a float32 is a double, a small whole number
    // an int.
    if (p_type.is_decimal())
      return builder.CreateFPExt(p_value, builder.getDoubleTy());
    if (p_type.is_whole() && bits_of(p_type) < 32)
      return is_unsigned_number(p_type)
                 ? builder.CreateZExt(p_value, builder.getInt32Ty())
                 : builder.CreateSExt(p_value, builder.getInt32Ty());
    return p_value;
  }

  // Calls a printf-style function whose format is written out. Integer
  // conversions get the `ll` length so every Ovis int is passed as 64 bits,
  // and values are converted to what each conversion shows.
  auto generate_format_arguments(const expression &p_call,
                                 const core::c_function &p_callee,
                                 std::vector<llvm::Value *> &p_values)
      -> void {
    const auto &format = p_call.arguments[*p_callee.format_index];
    const auto conversions = core::parse_format(format.text);
    string_type rewritten;
    std::size_t copied = 0;
    auto argument = p_callee.parameters.size();
    for (const auto &conversion : conversions) {
      const auto letter = conversion.conversion;
      auto spec_end = conversion.end - 1;
      if (string_view_type("diouxX").contains(letter)) {
        while (string_view_type("hlLqjzt").contains(format.text[spec_end - 1]))
          --spec_end;
        rewritten += format.text.substr(copied, spec_end - copied) + "ll";
        rewritten.push_back(letter);
      } else {
        rewritten += format.text.substr(copied, conversion.end - copied);
      }
      copied = conversion.end;

      if (conversion.takes_star_width) {
        const auto &width = p_call.arguments[argument++];
        p_values.push_back(
            builder.CreateTrunc(generate(width), builder.getInt32Ty()));
      }
      const auto &value_expression = p_call.arguments[argument++];
      auto *value = generate(value_expression);
      auto type = value_expression.type;
      // Numbers of a size are passed as C's variadic numbers: 64-bit whole
      // numbers (the `ll` above) and doubles.
      if (type.is_whole() && type.is_sized() &&
          *type.number_type() != core::c_type::size_type) {
        value = coerce(value, type, value_type::integer());
        type = value_type::integer();
      } else if (type.is_decimal() && type.is_sized()) {
        value = builder.CreateFPExt(value, builder.getDoubleTy());
        type = value_type::real();
      }
      if (string_view_type("diouxX").contains(letter)) {
        if (type.is_decimal())
          value = builder.CreateFPToSI(value, builder.getInt64Ty());
        else if (type == value_type::boolean())
          value = builder.CreateZExt(value, builder.getInt64Ty());
      } else if (letter == 'c') {
        value = type.is_decimal()
                    ? builder.CreateFPToSI(value, builder.getInt32Ty())
                : type == value_type::boolean()
                    ? builder.CreateZExt(value, builder.getInt32Ty())
                    : builder.CreateTrunc(value, builder.getInt32Ty());
      } else if (type.is_whole() && !string_view_type("sp").contains(letter)) {
        value = is_unsigned_number(type)
                    ? builder.CreateUIToFP(value, builder.getDoubleTy())
                    : builder.CreateSIToFP(value, builder.getDoubleTy());
      }
      p_values.push_back(value);
    }
    rewritten += format.text.substr(copied);
    p_values[*p_callee.format_index] =
        constant_text(rewritten);
  }

  // Checked builds: the value just stored in a variable is called by its
  // name in runtime errors ("the text in 'line' was freed on line 3"). The
  // prelude's own variables are no names the author knows.
  auto name_value(llvm::Value *p_value, const value_type &p_type,
                  const string_type &p_name) -> void {
    if (!naming || !(p_type == value_type::string() || p_type.is_list ||
                     p_type.is_record()))
      return;
    auto shown = p_name;
    while (shown.size() > 1 && shown.back() == '_')
      shown.pop_back();
    std::ranges::replace(shown, '_', ' ');
    auto &text = variable_names[shown];
    if (text == nullptr)
      text = builder.CreateGlobalString(shown, "ovis.name");
    runtime_call("ovis_name", builder.getVoidTy(), {p_value, text});
  }

  // Checked builds: what is given to C must not have been freed (C's
  // `strlen` would read freed memory without a word).
  auto check_live_for_c(llvm::Value *p_value, const value_type &p_type)
      -> void {
    if (checked && (p_type == value_type::string() || p_type.is_list ||
                    p_type.is_record()))
      runtime_call("ovis_live", builder.getVoidTy(), {p_value});
  }

  // Phrases as values (references.md §7, §7b).

  // A call through a phrase value: Ovis's convention, or C's for a C
  // phrase (numbers converted as for C functions).
  auto generate_phrase_call(const expression &p_call) -> llvm::Value * {
    const auto &arguments = p_call.arguments;
    const auto signature = *core::read_signature(arguments[0].type);
    const bool is_c = arguments[0].type.kind == core::pointer_kind::c_phrase;
    auto *const callee = generate(arguments[0]);
    if (checked)
      runtime_call("ovis_pointer_check", builder.getVoidTy(), {callee});
    std::vector<llvm::Type *> parameters;
    std::vector<llvm::Value *> values;
    for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
      const auto &argument = arguments[index + 1];
      auto *const value = generate(argument);
      if (is_c) {
        parameters.push_back(c_llvm_type(builder, signature.c_parameters[index]));
        values.push_back(to_c(value, argument.type, signature.c_parameters[index]));
      } else {
        parameters.push_back(type_of(signature.parameters[index]));
        values.push_back(coerce(value, argument.type, signature.parameters[index]));
      }
    }
    auto *const type = llvm::FunctionType::get(
        is_c ? c_llvm_type(builder, signature.c_result)
             : type_of(signature.result),
        parameters, false);
    auto *const result = builder.CreateCall(type, callee, values);
    restore_line();
    if (signature.result == value_type::nothing())
      return nullptr;
    return is_c ? from_c(result, signature.c_result) : result;
  }

  // A C function that calls the Ovis phrase `p_name` (references.md §7):
  // C's numbers converted to Ovis's and back. One per phrase and signature.
  auto c_thunk(const string_type &p_name, const value_type &p_c)
      -> llvm::Function * {
    const auto key = p_name + " " + p_c.pointee;
    if (const auto found = thunks.find(key); found != thunks.end())
      return found->second;
    const auto signature = *core::read_signature(p_c);
    auto *const target = functions.at(p_name);
    std::vector<llvm::Type *> parameters;
    for (const auto type : signature.c_parameters)
      parameters.push_back(c_llvm_type(builder, type));
    auto *const thunk = llvm::Function::Create(
        llvm::FunctionType::get(c_llvm_type(builder, signature.c_result),
                                parameters, false),
        llvm::Function::InternalLinkage, "ovis.c." + p_name, module.get());
    thunks[key] = thunk;
    const llvm::IRBuilderBase::InsertPointGuard guard(builder);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", thunk));
    std::vector<llvm::Value *> values;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      auto *value = from_c(thunk->getArg(static_cast<unsigned>(index)),
                           signature.c_parameters[index]);
      // A C `int` for an Ovis truth value.
      auto *const wanted = target->getFunctionType()->getParamType(
          static_cast<unsigned>(index));
      if (wanted->isIntegerTy(1) && value->getType()->isIntegerTy())
        value = builder.CreateICmpNE(
            value, llvm::ConstantInt::get(value->getType(), 0));
      values.push_back(value);
    }
    auto *const result = builder.CreateCall(target, values);
    if (signature.c_result == core::c_type::void_type) {
      builder.CreateRetVoid();
    } else {
      const auto from = target->getReturnType()->isIntegerTy(1)
                            ? value_type::boolean()
                        : target->getReturnType()->isDoubleTy()
                            ? value_type::real()
                        : target->getReturnType()->isIntegerTy()
                            ? value_type::integer()
                            : value_type::pointer();
      builder.CreateRet(to_c(result, from, signature.c_result));
    }
    return thunk;
  }

  // Raw pointers (references.md §5).

  // How a pointee is kept in memory.
  auto storage_of(const core::pointee_type &p_pointee) -> llvm::Type * {
    using kind = core::pointee_type::kind_type;
    switch (p_pointee.kind) {
    case kind::number:
      return c_llvm_type(builder, p_pointee.number);
    case kind::structure:
      return record_types.at(p_pointee.record);
    case kind::value:
      break;
    }
    return type_of(p_pointee.value);
  }

  auto size_of(const string_type &p_pointee) -> std::uint64_t {
    return module->getDataLayout().getTypeAllocSize(
        storage_of(*core::read_pointee(p_pointee)));
  }

  // Where a variable, a field or a list item is.
  auto address_of(const expression &p_place) -> llvm::Value * {
    if (p_place.kind == expression::kind_type::variable)
      return find_variable(p_place.text)->address;
    if (p_place.text == "field")
      return field_address(generate(p_place.arguments[0]),
                           p_place.arguments[0].type.record, p_place.fields[0]);
    return element_address(generate(p_place.arguments[0]),
                           generate(p_place.arguments[1]), p_place.type);
  }

  auto generate_raw(const expression &p_call) -> llvm::Value * {
    const auto &name = p_call.text;
    const auto &arguments = p_call.arguments;
    if (name == "size-of") {
      // A record's size is its struct's (C's `sizeof(z_stream)`); `size of
      // a pointer` is the pointer's.
      const auto pointee = *core::read_pointee(p_call.fields[0]);
      if (pointee.kind == core::pointee_type::kind_type::value &&
          pointee.value.is_record() && records.contains(pointee.value.record))
        return builder.getInt64(module->getDataLayout().getTypeAllocSize(
            record_types.at(pointee.value.record)));
      return builder.getInt64(size_of(p_call.fields[0]));
    }
    if (name == "view")
      return runtime_call("ovis_list_view", builder.getPtrTy(),
                          {generate(arguments[0]), generate(arguments[1])});
    // Room for items (generics.md §5).
    const auto count = [&](std::size_t p_index) {
      return coerce(generate(arguments[p_index]), arguments[p_index].type,
                    value_type::integer());
    };
    if (name == "alloc")
      return runtime_call("ovis_alloc_items", builder.getPtrTy(),
                          {count(0), builder.getInt64(size_of(p_call.fields[0]))});
    if (name == "alloc-more")
      return runtime_call("ovis_alloc_more", builder.getPtrTy(),
                          {generate(arguments[0]), count(1), count(2),
                           builder.getInt64(size_of(p_call.fields[0]))});
    if (name == "cast") {
      auto *const value = generate(arguments[0]);
      const auto &to = *p_call.type_argument;
      // A list as a pointer or text: its items.
      if (arguments[0].type.is_list)
        return runtime_call("ovis_list_data", builder.getPtrTy(), {value});
      if (to == value_type::integer())
        return builder.CreatePtrToInt(value, builder.getInt64Ty());
      if (arguments[0].type == value_type::integer())
        return builder.CreateIntToPtr(value, builder.getPtrTy());
      return value;
    }
    auto *const pointer = generate(arguments[0]);
    if (name == "offset") {
      const auto scale =
          p_call.fields.empty() ? size_of(arguments[0].type.pointee)
                                : size_of(p_call.fields[0]);
      auto *const bytes =
          builder.CreateMul(generate(arguments[1]), builder.getInt64(scale));
      return builder.CreateInBoundsGEP(builder.getInt8Ty(), pointer, bytes);
    }
    // Checked builds: inside room Ovis made, and not freed.
    if (checked)
      runtime_call("ovis_room_check", builder.getVoidTy(),
                   {pointer, builder.getInt64(size_of(p_call.fields[0]))});
    const auto pointee = *core::read_pointee(p_call.fields[0]);
    using kind = core::pointee_type::kind_type;
    if (name == "load") {
      if (pointee.kind == kind::structure)
        return pointer; // the record is its memory
      auto *const value = builder.CreateLoad(storage_of(pointee), pointer);
      return pointee.kind == kind::number ? from_c(value, pointee.number)
                                          : value;
    }
    // store
    auto *const value = generate(arguments[1]);
    builder.CreateStore(
        pointee.kind == kind::number
            ? to_c(value, arguments[1].type, pointee.number)
            : coerce(value, arguments[1].type, pointee.value),
        pointer);
    return nullptr;
  }

  auto generate_c_call(const expression &p_call,
                       const core::c_function &p_callee) -> llvm::Value * {
    std::vector<llvm::Type *> parameter_types;
    for (const auto parameter : p_callee.parameters)
      parameter_types.push_back(c_llvm_type(builder, parameter));
    // A struct by value goes through the C wrapper written for it.
    const auto declaration = module->getOrInsertFunction(
        p_callee.needs_wrapper() ? "ovis_w_" + p_callee.name : p_callee.name,
        llvm::FunctionType::get(c_llvm_type(builder, p_callee.result),
                                parameter_types, p_callee.is_variadic));

    const auto &arguments = p_call.arguments;
    std::vector<llvm::Value *> values;
    // (ref x) through a pointer to a narrower C type (`int *`, `float *`)
    // goes through a temporary of that type, copied back after the call.
    struct write_back {
      llvm::Value *temporary;
      core::c_type type;
      variable *target;
    };
    std::vector<write_back> write_backs;
    for (std::size_t index = 0; index < p_callee.parameters.size(); ++index) {
      const auto &argument = arguments[index];
      const auto pointee = index < p_callee.pointees.size()
                               ? p_callee.pointees[index]
                               : std::nullopt;
      if (argument.kind == expression::kind_type::call &&
          argument.text == "ref" && pointee &&
          argument.arguments[0].kind == expression::kind_type::variable) {
        auto *const target = find_variable(argument.arguments[0].text);
        auto *const type = c_llvm_type(builder, *pointee);
        if (type == type_of(target->type)) {
          values.push_back(target->address);
          continue;
        }
        auto *const temporary = create_entry_alloca(type, "ref");
        builder.CreateStore(
            to_c(builder.CreateLoad(type_of(target->type), target->address),
                 target->type, *pointee),
            temporary);
        values.push_back(temporary);
        write_backs.push_back({temporary, *pointee, target});
        continue;
      }
      // An Ovis phrase where C wants a function: the C function for it.
      if (argument.type.kind == core::pointer_kind::phrase &&
          argument.type.is_phrase()) {
        values.push_back(c_thunk(argument.arguments[0].text,
                                 core::c_parameter_type(p_callee, index)));
        continue;
      }
      auto *value = generate(argument);
      check_live_for_c(value, argument.type);
      if (index < p_callee.by_value.size() && p_callee.by_value[index])
        runtime_call("ovis_record_check", builder.getVoidTy(), {value});
      // A list gives C its items.
      if (argument.type.is_list)
        value = runtime_call("ovis_list_data", builder.getPtrTy(), {value});
      values.push_back(to_c(value, argument.type, p_callee.parameters[index]));
    }
    if (p_callee.format_index &&
        arguments[*p_callee.format_index].kind ==
            expression::kind_type::string) {
      generate_format_arguments(p_call, p_callee, values);
    } else {
      for (std::size_t index = p_callee.parameters.size();
           index < arguments.size(); ++index) {
        auto *const value = generate(arguments[index]);
        check_live_for_c(value, arguments[index].type);
        values.push_back(to_c_variadic(value, arguments[index].type));
      }
    }
    auto *const result = builder.CreateCall(declaration, values);
    for (const auto &item : write_backs)
      builder.CreateStore(
          from_c(builder.CreateLoad(c_llvm_type(builder, item.type),
                                    item.temporary),
                 item.type),
          item.target->address);
    return p_callee.result == core::c_type::void_type
               ? nullptr
               : from_c(result, p_callee.result);
  }

  auto generate_call(const expression &p_call) -> llvm::Value * {
    const auto &name = p_call.text;
    const auto &arguments = p_call.arguments;

    if ((name == "+" && p_call.type == value_type::string()) ||
        name == "concat")
      return generate_concat(p_call);
    if (name == "to_str") {
      // An array of bytes: its own text, up to the first zero byte, copied.
      if (arguments[0].type.is_c_array())
        return runtime_call(
            "ovis_bytes_text", builder.getPtrTy(),
            {runtime_call("ovis_list_data", builder.getPtrTy(),
                          {generate(arguments[0])}),
             runtime_call("ovis_list_len", builder.getInt64Ty(),
                          {generate(arguments[0])})});
      return to_text(generate(arguments[0]), arguments[0].type);
    }
    if (name == "ref")
      return address_of(arguments[0]);
    if (name == "load" || name == "store" || name == "offset" ||
        name == "cast" || name == "size-of" || name == "view" ||
        name == "alloc" || name == "alloc-more")
      return generate_raw(p_call);
    if (name == "phrase-of")
      return functions.at(arguments[0].text);
    if (name == "call")
      return generate_phrase_call(p_call);
    if (name == "new")
      return generate_new(p_call);
    if (name == "field") {
      const auto &record = arguments[0].type.record;
      return load_field(generate(arguments[0]), record,
                        *records.at(record)->find(p_call.fields[0]));
    }
    if (name == "+" || name == "-" || name == "*" || name == "/" ||
        name == "%" || name == "div" || name == "min" || name == "max")
      return generate_arithmetic(p_call);
    if (name == "=" || name == "!=" || name == "<" || name == "<=" ||
        name == ">" || name == ">=")
      return generate_comparison(p_call);
    if (name == "and" || name == "or")
      return generate_logical(p_call);
    if (name == "not")
      return builder.CreateNot(generate(arguments[0]));

    if (name == "int") {
      auto *const value = generate(arguments[0]);
      if (arguments[0].type.is_decimal())
        return builder.CreateFPToSI(
            builder.CreateFPExt(value, builder.getDoubleTy()),
            builder.getInt64Ty());
      if (arguments[0].type == value_type::boolean())
        return builder.CreateZExt(value, builder.getInt64Ty());
      return coerce(value, arguments[0].type, value_type::integer());
    }
    // `(as T X)`, `(wrap T X)`, `(clamp T X)`: a number as another type.
    if (name == "as" || name == "wrap-to" || name == "clamp-to")
      return convert_number(generate(arguments[0]), arguments[0].type,
                            p_call.type,
                            name == "as"        ? conversion::checked
                            : name == "wrap-to" ? conversion::wrap
                                                : conversion::clamp);
    if (core::is_bit_operation(name))
      return generate_bits(p_call);
    if (name == "wrapping" || name == "saturating") {
      const auto outer = arithmetic;
      arithmetic = name == "wrapping" ? arithmetic_wrapping : arithmetic_saturating;
      auto *const value = generate(arguments[0]);
      arithmetic = outer;
      return value;
    }
    if (name == "float-bits" || name == "bits-float")
      return builder.CreateBitCast(generate(arguments[0]), type_of(p_call.type));
    if (name.starts_with("to-bytes") || name.starts_with("from-bytes"))
      return generate_bytes(p_call);
    if (name == "to-hex" || name == "to-binary")
      return generate_digits(p_call);
    if (name == "from-hex")
      return runtime_call("ovis_from_hex", builder.getInt64Ty(),
                          {generate(arguments[0])});

    auto real_argument = [&](std::size_t p_index) {
      return coerce(generate(arguments[p_index]), arguments[p_index].type,
                    value_type::real());
    };
    if (name == "float")
      return real_argument(0);
    if (name == "sqrt")
      return intrinsic(llvm::Intrinsic::sqrt, {real_argument(0)});
    if (name == "floor")
      return intrinsic(llvm::Intrinsic::floor, {real_argument(0)});
    if (name == "pow")
      return intrinsic(llvm::Intrinsic::pow,
                       {real_argument(0), real_argument(1)});
    if (name == "abs") {
      auto *const value = coerce(generate(arguments[0]), arguments[0].type,
                                 p_call.type);
      if (p_call.type == value_type::real())
        return intrinsic(llvm::Intrinsic::fabs, {value});
      if (p_call.type.is_decimal())
        return builder.CreateSelect(
            builder.CreateFCmpOLT(value, llvm::ConstantFP::get(value->getType(), 0.0)),
            builder.CreateFNeg(value), value);
      if (is_unsigned_number(p_call.type))
        return value;
      return builder.CreateSelect(
          builder.CreateICmpSLT(value, llvm::ConstantInt::get(value->getType(), 0)),
          builder.CreateNeg(value), value);
    }

    if (name == "len")
      return runtime_call(arguments[0].type.is_map() ? "ovis_map_len"
                                                     : "ovis_list_len",
                          builder.getInt64Ty(), {generate(arguments[0])});
    if (name.starts_with("map-"))
      return generate_map(p_call);
    if (name == "get") {
      auto *const list = generate(arguments[0]);
      auto *const index = generate(arguments[1]);
      const auto item = item_storage(arguments[0].type);
      return load_element(element_address(list, index, item), item);
    }
    if (name == "list" || name == "array") {
      const auto element = item_storage(p_call.type);
      const bool is_literal = name == "list";
      auto *const count = is_literal ? builder.getInt64(arguments.size())
                                     : generate(arguments[0]);
      auto *const list = runtime_call("ovis_list_new", builder.getPtrTy(),
                                      {element_size(element), count});
      if (is_literal) {
        for (std::size_t index = 0; index < arguments.size(); ++index) {
          auto *const value = coerce(generate(arguments[index]),
                                     arguments[index].type, item_value(element));
          store_element(
              element_address(list, builder.getInt64(index), element), value,
              element);
        }
      } else if (element == value_type::string() || element.is_list) {
        // Zeroed memory is a null pointer; make string arrays hold "" and
        // arrays of lists hold empty lists.
        for_range(builder.getInt64(0),
                  builder.CreateSub(count, builder.getInt64(1)),
                  [&](llvm::Value *p_index) {
                    store_element(element_address(list, p_index, element),
                                  zero_of(element), element);
                  });
      }
      return list;
    }
    if (name == "append") {
      const auto element = item_storage(arguments[0].type);
      auto *const list = generate(arguments[0]);
      auto *const value =
          coerce(generate(arguments[1]), arguments[1].type, item_value(element));
      auto *const address = runtime_call("ovis_list_push", builder.getPtrTy(),
                                         {list, element_size(element)});
      store_element(address, value, element);
      return nullptr;
    }
    if (name == "read_int")
      return runtime_call("ovis_read_int", builder.getInt64Ty(), {});
    if (name == "read_float")
      return runtime_call("ovis_read_float", builder.getDoubleTy(), {});
    if (name == "random")
      return runtime_call("ovis_random", builder.getInt64Ty(),
                          {generate(arguments[0]), generate(arguments[1])});
    if (name == "read_line")
      return runtime_call("ovis_read_line", builder.getPtrTy(), {});
    if (name == "substring")
      return runtime_call("ovis_substring", builder.getPtrTy(),
                          {generate(arguments[0]), generate(arguments[1]),
                           generate(arguments[2])});
    if (name == "char_at")
      return runtime_call("ovis_char_at", builder.getInt64Ty(),
                          {generate(arguments[0]), generate(arguments[1])});
    if (name == "set-char") {
      runtime_call("ovis_set_char", builder.getVoidTy(),
                   {generate(arguments[0]), generate(arguments[1]),
                    generate(arguments[2])});
      return nullptr;
    }
    if (name == "chr" || name == "buffer")
      return runtime_call(name == "chr" ? "ovis_chr" : "ovis_buffer",
                          builder.getPtrTy(), {generate(arguments[0])});
    if (name == "sort") {
      // Numbers of a size: by their C number (numbers.md §5).
      if (arguments[0].type.is_c_array()) {
        static const std::map<string_view_type, int> codes = {
            {"i8", 0}, {"u8", 1},  {"i16", 2}, {"u16", 3},
            {"i32", 4}, {"u32", 5}, {"u64", 6}, {"f32", 7}};
        runtime_call("ovis_list_sort_number", builder.getVoidTy(),
                     {generate(arguments[0]),
                      builder.getInt32(codes.at(arguments[0].type.pointee))});
        return nullptr;
      }
      runtime_call("ovis_list_sort", builder.getVoidTy(),
                   {generate(arguments[0]), list_kind(arguments[0].type)});
      return nullptr;
    }
    if (name == "new-memory")
      return runtime_call("ovis_memory_new", builder.getPtrTy(), {});
    if (name == "clear-memory") {
      runtime_call("ovis_memory_clear", builder.getVoidTy(),
                   {generate(arguments[0])});
      return nullptr;
    }
    if (name == "free") {
      const auto &type = arguments[0].type;
      auto *const value = generate(arguments[0]);
      const char *function = type.is_list      ? "ovis_free_list"
                             : type.is_map()    ? "ovis_free_map"
                             : type.is_record() ? "ovis_free_record"
                             : type == value_type::string() ? "ovis_free_text"
                             : type == value_type::memory() ? "ovis_memory_free"
                                                            : "ovis_free_pointer";
      runtime_call(function, builder.getVoidTy(), {value});
      return nullptr;
    }
    if (name == "copy") {
      // A deep copy of everything reachable, in the current memory.
      runtime_call("ovis_copy_begin", builder.getVoidTy(), {});
      auto *const result = keep_value(generate(arguments[0]), arguments[0].type);
      runtime_call("ovis_copy_end", builder.getVoidTy(), {});
      return result;
    }
    if (name == "keep") {
      runtime_call("ovis_keep_begin", builder.getVoidTy(), {});
      auto *const result = keep_value(generate(arguments[0]), arguments[0].type);
      runtime_call("ovis_keep_end", builder.getVoidTy(), {});
      return result;
    }
    if (name == "remove-at") {
      runtime_call("ovis_list_remove", builder.getVoidTy(),
                   {generate(arguments[0]), generate(arguments[1]),
                    element_size(item_storage(arguments[0].type))});
      return nullptr;
    }
    if (name == "reverse") {
      runtime_call("ovis_list_reverse", builder.getVoidTy(),
                   {generate(arguments[0]),
                    element_size(item_storage(arguments[0].type))});
      return nullptr;
    }

    if (method_owner.contains(name) && arguments[0].type.is_record() &&
        traits.contains(arguments[0].type.record))
      return generate_dispatch(p_call);
    if (!functions.contains(name))
      return generate_c_call(p_call, *c_functions.at(name));
    auto *const callee = functions.at(name);
    const auto &declaration = *find_function(name);
    std::vector<llvm::Value *> values;
    for (std::size_t index = 0; index < arguments.size(); ++index)
      values.push_back(coerce(generate(arguments[index]),
                              arguments[index].type,
                              declaration.parameters[index].type));
    auto *const result = builder.CreateCall(callee, values);
    restore_line();
    return result;
  }

  auto generate(const expression &p_expression) -> llvm::Value * {
    using kind = expression::kind_type;
    switch (p_expression.kind) {
    case kind::integer:
    case kind::real:
      return number_constant(p_expression);
    case kind::boolean:
      return builder.getInt1(p_expression.boolean);
    case kind::string:
      return constant_text(p_expression.text);
    case kind::variable: {
      const auto *const found = find_variable(p_expression.text);
      if (found == nullptr)
        return generate_constant(*core::find_constant(source, p_expression.text));
      return builder.CreateLoad(type_of(found->type), found->address,
                                p_expression.text);
    }
    case kind::call:
      return generate_call(p_expression);
    }
    return nullptr;
  }

  // Statements.

  // Calls `p_body` for each index from `p_first` to `p_last` inclusive.
  template <typename t_body_type>
  auto for_range(llvm::Value *p_first, llvm::Value *p_last,
                 t_body_type p_body) -> void {
    auto *const counter =
        create_entry_alloca(builder.getInt64Ty(), "range.index");
    builder.CreateStore(p_first, counter);
    auto *const condition_block =
        llvm::BasicBlock::Create(context, "range.condition", current_function);
    auto *const body_block =
        llvm::BasicBlock::Create(context, "range.body", current_function);
    auto *const end_block =
        llvm::BasicBlock::Create(context, "range.end", current_function);
    builder.CreateBr(condition_block);
    builder.SetInsertPoint(condition_block);
    auto *const index = builder.CreateLoad(builder.getInt64Ty(), counter);
    builder.CreateCondBr(builder.CreateICmpSLE(index, p_last), body_block,
                         end_block);
    builder.SetInsertPoint(body_block);
    p_body(index);
    builder.CreateStore(builder.CreateAdd(index, builder.getInt64(1)), counter);
    builder.CreateBr(condition_block);
    builder.SetInsertPoint(end_block);
  }

  // Not getTerminator(): from LLVM 23 it assumes the block has one and
  // gives back its last instruction, so every non-empty block looked done.
  auto is_terminated() -> bool {
    const auto *const block = builder.GetInsertBlock();
    return !block->empty() && block->back().isTerminator();
  }

  // Code after break, continue or return still needs somewhere to go.
  auto start_unreachable_block() -> void {
    builder.SetInsertPoint(
        llvm::BasicBlock::Create(context, "unreachable", current_function));
  }

  // Emits the deferred statements of the frames from the top down to
  // `p_depth`, last first, for an exit that leaves them. The frames are cut
  // below each one while its statements are emitted, so a `defer` inside a
  // deferred statement belongs to that statement and nothing runs twice.
  auto emit_frames(std::size_t p_depth) -> void {
    const auto saved = frames;
    for (auto index = saved.size(); index > p_depth; --index) {
      frames.resize(index - 1);
      const auto &deferred = saved[index - 1];
      for (auto action = deferred.rbegin(); action != deferred.rend();
           ++action) {
        if (action->deferred != nullptr) {
          generate_block(action->deferred->body);
          continue;
        }
        runtime_call("ovis_memory_pop", builder.getVoidTy(), {});
        store_line(action->line);
        if (action->frees)
          runtime_call("ovis_memory_free", builder.getVoidTy(),
                       {action->memory});
      }
    }
    frames = saved;
  }

  auto generate_block(const std::vector<statement> &p_body,
                      std::optional<exit_action> p_first = std::nullopt)
      -> void {
    scopes.emplace_back();
    frames.emplace_back();
    if (p_first)
      frames.back().push_back(*p_first);
    for (const auto &statement : p_body)
      generate(statement);
    if (!is_terminated())
      emit_frames(frames.size() - 1);
    frames.pop_back();
    scopes.pop_back();
  }

  auto generate_loop(llvm::BasicBlock *p_condition_block,
                     llvm::BasicBlock *p_continue_block,
                     llvm::BasicBlock *p_end_block,
                     const std::vector<statement> &p_body) -> void {
    loops.push_back({p_continue_block, p_end_block, frames.size()});
    generate_block(p_body);
    loops.pop_back();
    if (!is_terminated())
      builder.CreateBr(p_continue_block);
    if (p_continue_block != p_condition_block) {
      builder.SetInsertPoint(p_continue_block);
    }
  }

  // How the runtime tells list element types apart.
  auto list_kind(const value_type &p_type) -> llvm::Value * {
    return builder.getInt32(static_cast<int>(p_type.base) -
                            static_cast<int>(base_type::integer));
  }

  auto print_text(string_view_type p_text) -> void {
    runtime_call("ovis_print_str", builder.getVoidTy(),
                 {constant_text(string_type(p_text))});
  }

  // A record inside another value is bracketed, so its commas stay apart
  // from the outer value's: `[(a book with title "Dune", pages 412)]`.
  auto print_inner(llvm::Value *p_value, const value_type &p_type) -> void {
    if (p_type.is_record()) {
      builder.CreateCall(record_printer(p_type.record),
                         {p_value, builder.getInt1(true)});
    } else if (p_type == value_type::string()) {
      runtime_call("ovis_print_quoted", builder.getVoidTy(), {p_value});
    } else {
      print_value(p_value, p_type);
    }
  }

  // `ovis.print.NAME(record, is_inner)`: prints a record as it is written,
  // `a book with title "Dune", pages 412`, or `nothing`. A function, so a
  // record holding its own kind of record compiles.
  // `keep`: a copy, in the memory outside the innermost `with`, of the parts
  // of a value that were made in it. Text, lists and records are followed;
  // what is shared or circular is copied once (the runtime's map).
  // Maps (`(map K V)`): every key and value is 8 bytes in the runtime's
  // table, a number's or decimal's bits or a pointer.
  // A number of a size is kept as its 64-bit number: a float32 as a
  // decimal, a byte as a whole number.
  auto to_bits(llvm::Value *p_value, const value_type &p_type) -> llvm::Value * {
    if (p_type.is_sized())
      return to_bits(coerce(p_value, p_type,
                            p_type.is_decimal() ? value_type::real()
                            : *p_type.number_type() == core::c_type::size_type
                                ? p_type
                                : value_type::integer()),
                     p_type.is_decimal() ? value_type::real() : value_type::integer());
    if (p_type == value_type::real())
      return builder.CreateBitCast(p_value, builder.getInt64Ty());
    if (p_type == value_type::boolean())
      return builder.CreateZExt(p_value, builder.getInt64Ty());
    if (p_value->getType()->isPointerTy())
      return builder.CreatePtrToInt(p_value, builder.getInt64Ty());
    return p_value;
  }

  auto from_bits(llvm::Value *p_bits, const value_type &p_type) -> llvm::Value * {
    if (p_type.is_sized() && p_type.is_decimal())
      return builder.CreateFPTrunc(
          builder.CreateBitCast(p_bits, builder.getDoubleTy()), type_of(p_type));
    if (p_type.is_sized())
      return builder.CreateTrunc(p_bits, type_of(p_type));
    if (p_type == value_type::real())
      return builder.CreateBitCast(p_bits, builder.getDoubleTy());
    if (p_type == value_type::boolean())
      return builder.CreateICmpNE(p_bits, builder.getInt64(0));
    if (type_of(p_type)->isPointerTy())
      return builder.CreateIntToPtr(p_bits, builder.getPtrTy());
    return p_bits;
  }

  // A map's keys as a new list: the runtime's, whose items are 64 bits; for
  // keys of a size, made here at their size, each narrowed back.
  auto map_keys(llvm::Value *p_map, const value_type &p_type) -> llvm::Value * {
    const auto key = p_type.map_key();
    if (!key.is_sized())
      return runtime_call("ovis_map_keys", builder.getPtrTy(), {p_map});
    const auto element = item_storage(value_type::list_of(key));
    auto *const list =
        runtime_call("ovis_list_new", builder.getPtrTy(),
                     {element_size(element), builder.getInt64(0)});
    for_each_entry(p_map, [&](llvm::Value *p_at) {
      auto *const value = from_bits(
          runtime_call("ovis_map_key_at", builder.getInt64Ty(), {p_map, p_at}),
          key);
      auto *const address = runtime_call("ovis_list_push", builder.getPtrTy(),
                                         {list, element_size(element)});
      store_element(address, coerce(value, key, item_value(element)), element);
    });
    return list;
  }

  auto generate_map(const expression &p_call) -> llvm::Value * {
    const auto &name = p_call.text;
    const auto &arguments = p_call.arguments;
    if (name == "map-new")
      return zero_of(*p_call.type_argument);
    const auto &type = arguments[0].type;
    auto *const map = generate(arguments[0]);
    if (name == "map-keys")
      return map_keys(map, type);
    auto *key_value =
        coerce(generate(arguments[1]), arguments[1].type, type.map_key());
    // -0.0 and 0.0 are one key: adding 0.0 makes the first the second.
    if (type.map_key().is_decimal())
      key_value = builder.CreateFAdd(
          key_value, llvm::ConstantFP::get(key_value->getType(), 0.0));
    auto *const key = to_bits(key_value, type.map_key());
    if (name == "map-put") {
      runtime_call("ovis_map_put", builder.getVoidTy(),
                   {map, key,
                    to_bits(coerce(generate(arguments[2]), arguments[2].type,
                                   type.map_value()),
                            type.map_value())});
      return nullptr;
    }
    if (name == "map-get")
      return from_bits(runtime_call("ovis_map_get", builder.getInt64Ty(),
                                    {map, key}),
                       type.map_value());
    if (name == "map-has")
      return builder.CreateICmpNE(
          runtime_call("ovis_map_has", builder.getInt32Ty(), {map, key}),
          builder.getInt32(0));
    runtime_call("ovis_map_remove", builder.getVoidTy(), {map, key});
    return nullptr;
  }

  // Runs `p_body` with each entry's place in the map, in order.
  template <typename t_body_type>
  auto for_each_entry(llvm::Value *p_map, t_body_type p_body) -> void {
    for_range(builder.getInt64(0),
              builder.CreateSub(runtime_call("ovis_map_places",
                                             builder.getInt64Ty(), {p_map}),
                                builder.getInt64(1)),
              [&](llvm::Value *p_at) {
                auto *const entry_block = llvm::BasicBlock::Create(
                    context, "map.entry", current_function);
                auto *const next_block = llvm::BasicBlock::Create(
                    context, "map.next", current_function);
                builder.CreateCondBr(
                    builder.CreateICmpNE(
                        runtime_call("ovis_map_alive", builder.getInt32Ty(),
                                     {p_map, p_at}),
                        builder.getInt32(0)),
                    entry_block, next_block);
                builder.SetInsertPoint(entry_block);
                p_body(p_at);
                builder.CreateBr(next_block);
                builder.SetInsertPoint(next_block);
              });
  }

  // `{"a": 1, "b": 2}`.
  auto print_map(llvm::Value *p_map, const value_type &p_type) -> void {
    auto *const first = create_entry_alloca(builder.getInt1Ty(), "map.first");
    builder.CreateStore(builder.getInt1(true), first);
    print_text("{");
    for_each_entry(p_map, [&](llvm::Value *p_at) {
      auto *const comma_block =
          llvm::BasicBlock::Create(context, "map.comma", current_function);
      auto *const item_block =
          llvm::BasicBlock::Create(context, "map.item", current_function);
      builder.CreateCondBr(builder.CreateLoad(builder.getInt1Ty(), first),
                           item_block, comma_block);
      builder.SetInsertPoint(comma_block);
      print_text(", ");
      builder.CreateBr(item_block);
      builder.SetInsertPoint(item_block);
      builder.CreateStore(builder.getInt1(false), first);
      print_inner(from_bits(runtime_call("ovis_map_key_at", builder.getInt64Ty(),
                                         {p_map, p_at}),
                            p_type.map_key()),
                  p_type.map_key());
      print_text(": ");
      print_inner(from_bits(runtime_call("ovis_map_value_at",
                                         builder.getInt64Ty(), {p_map, p_at}),
                            p_type.map_value()),
                  p_type.map_value());
    });
    print_text("}");
  }

  auto keep_value(llvm::Value *p_value, const value_type &p_type) -> llvm::Value * {
    if (p_type.is_map()) {
      // The table, then what its entries hold: text keys and values.
      auto *const copy = runtime_call("ovis_keep_map", builder.getPtrTy(), {p_value});
      const auto key = p_type.map_key();
      const auto value = p_type.map_value();
      const bool keeps_key = key == value_type::string();
      const bool keeps_value = value == value_type::string() || value.is_list ||
                               value.is_record() || value.is_map();
      if (keeps_key || keeps_value)
        for_each_entry(copy, [&](llvm::Value *p_at) {
          if (keeps_key)
            runtime_call(
                "ovis_map_set_key_at", builder.getVoidTy(),
                {copy, p_at,
                 to_bits(keep_value(from_bits(runtime_call("ovis_map_key_at",
                                                           builder.getInt64Ty(),
                                                           {copy, p_at}),
                                              key),
                                    key),
                         key)});
          if (keeps_value)
            runtime_call(
                "ovis_map_set_value_at", builder.getVoidTy(),
                {copy, p_at,
                 to_bits(keep_value(from_bits(runtime_call("ovis_map_value_at",
                                                           builder.getInt64Ty(),
                                                           {copy, p_at}),
                                              value),
                                    value),
                         value)});
        });
      return copy;
    }
    if (p_type == value_type::string())
      return runtime_call("ovis_keep_text", builder.getPtrTy(), {p_value});
    if (p_type.is_record())
      return builder.CreateCall(record_keeper(p_type.record), {p_value});
    if (!p_type.is_list)
      return p_value;
    const auto element = item_storage(p_type);
    auto *const copy = runtime_call("ovis_keep_list", builder.getPtrTy(),
                                    {p_value, element_size(element)});
    if (element == value_type::string()) {
      runtime_call("ovis_keep_list_text", builder.getVoidTy(), {p_value, copy});
    } else if (element.is_record() || element.is_list) {
      // A list that was not copied (it is from outside) keeps its items.
      auto *const items_block =
          llvm::BasicBlock::Create(context, "keep.items", current_function);
      auto *const done_block =
          llvm::BasicBlock::Create(context, "keep.done", current_function);
      builder.CreateCondBr(builder.CreateICmpNE(copy, p_value), items_block,
                           done_block);
      builder.SetInsertPoint(items_block);
      for_range(builder.getInt64(0),
                builder.CreateSub(runtime_call("ovis_list_len",
                                               builder.getInt64Ty(), {copy}),
                                  builder.getInt64(1)),
                [&](llvm::Value *p_index) {
                  auto *const address = element_address(copy, p_index, element);
                  store_element(address,
                                keep_value(load_element(address, element), element),
                                element);
                });
      builder.CreateBr(done_block);
      builder.SetInsertPoint(done_block);
    }
    return copy;
  }

  auto record_keeper(const string_type &p_name) -> llvm::Function * {
    if (const auto found = record_keepers.find(p_name);
        found != record_keepers.end())
      return found->second;
    auto *const keeper = llvm::Function::Create(
        llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy()}, false),
        llvm::Function::InternalLinkage, "ovis.keep." + p_name, module.get());
    record_keepers[p_name] = keeper;

    const llvm::IRBuilderBase::InsertPointGuard guard(builder);
    auto *const outer_function = current_function;
    current_function = keeper;
    auto *const record = keeper->getArg(0);
    auto *const entry = llvm::BasicBlock::Create(context, "entry", keeper);
    builder.SetInsertPoint(entry);
    if (traits.contains(p_name)) {
      // Known only as a trait: keep the record it turns out to be.
      auto *const tag = builder.CreateSelect(
          builder.CreateIsNull(record), builder.getInt64(~0ULL),
          builder.CreateLoad(builder.getInt64Ty(), record));
      for (const auto &[record_name, declaration] : records) {
        if (!implemented.contains({p_name, record_name}))
          continue;
        auto *const hit = llvm::BasicBlock::Create(context, "hit", keeper);
        auto *const miss = llvm::BasicBlock::Create(context, "miss", keeper);
        builder.CreateCondBr(
            builder.CreateICmpEQ(tag, builder.getInt64(record_tags.at(record_name))),
            hit, miss);
        builder.SetInsertPoint(hit);
        builder.CreateRet(
            builder.CreateCall(record_keeper(record_name), {record}));
        builder.SetInsertPoint(miss);
      }
      builder.CreateRet(record);
      current_function = outer_function;
      return keeper;
    }
    auto *const check_block = llvm::BasicBlock::Create(context, "check", keeper);
    auto *const copy_block = llvm::BasicBlock::Create(context, "copy", keeper);
    auto *const leave_block = llvm::BasicBlock::Create(context, "leave", keeper);
    auto *const owned = runtime_call("ovis_keep_owned", builder.getInt32Ty(), {record});
    builder.CreateCondBr(builder.CreateICmpNE(owned, builder.getInt32(0)),
                         check_block, leave_block);
    builder.SetInsertPoint(leave_block);
    builder.CreateRet(record);
    builder.SetInsertPoint(check_block);
    auto *const seen = runtime_call("ovis_keep_seen", builder.getPtrTy(), {record});
    auto *const seen_block = llvm::BasicBlock::Create(context, "seen", keeper);
    builder.CreateCondBr(builder.CreateIsNull(seen), copy_block, seen_block);
    builder.SetInsertPoint(seen_block);
    builder.CreateRet(seen);
    builder.SetInsertPoint(copy_block);
    auto *const type = record_types.at(p_name);
    auto *const copy = runtime_call("ovis_keep_copy", builder.getPtrTy(),
                                    {record, llvm::ConstantExpr::getSizeOf(type)});
    const auto &declaration = *records.at(p_name);
    // A C struct's bytes: its pointers still point where they did.
    if (declaration.is_c) {
      builder.CreateRet(copy);
      current_function = outer_function;
      return keeper;
    }
    for (std::size_t index = 0; index < declaration.fields.size(); ++index) {
      const auto &field = declaration.fields[index];
      if (!(field.type == value_type::string() || field.type.is_list ||
            field.type.is_record()))
        continue;
      auto *const address =
          builder.CreateStructGEP(type, copy, field_index(p_name, field.name));
      builder.CreateStore(
          keep_value(builder.CreateLoad(type_of(field.type), address), field.type),
          address);
    }
    builder.CreateRet(copy);
    current_function = outer_function;
    return keeper;
  }

  auto record_printer(const string_type &p_name) -> llvm::Function * {
    if (const auto found = record_printers.find(p_name);
        found != record_printers.end())
      return found->second;
    auto *const printer = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(),
                                {builder.getPtrTy(), builder.getInt1Ty()},
                                false),
        llvm::Function::InternalLinkage, "ovis.print." + p_name, module.get());
    record_printers[p_name] = printer;

    const llvm::IRBuilderBase::InsertPointGuard guard(builder);
    auto *const outer_function = current_function;
    current_function = printer;
    auto *const record = printer->getArg(0);
    auto *const is_inner = printer->getArg(1);
    if (traits.contains(p_name)) {
      // Known only as a trait: print the record it turns out to be.
      auto *const entry = llvm::BasicBlock::Create(context, "entry", printer);
      auto *const empty_block =
          llvm::BasicBlock::Create(context, "empty", printer);
      auto *const done_block = llvm::BasicBlock::Create(context, "done", printer);
      builder.SetInsertPoint(entry);
      auto *const next_block = llvm::BasicBlock::Create(context, "next", printer);
      builder.CreateCondBr(builder.CreateIsNull(record), empty_block, next_block);
      builder.SetInsertPoint(empty_block);
      print_text("nothing");
      builder.CreateBr(done_block);
      builder.SetInsertPoint(next_block);
      auto *const tag = builder.CreateLoad(builder.getInt64Ty(), record);
      for (const auto &[record_name, declaration] : records) {
        if (!implemented.contains({p_name, record_name}))
          continue;
        auto *const hit = llvm::BasicBlock::Create(context, "hit", printer);
        auto *const miss = llvm::BasicBlock::Create(context, "miss", printer);
        builder.CreateCondBr(
            builder.CreateICmpEQ(tag, builder.getInt64(record_tags.at(record_name))),
            hit, miss);
        builder.SetInsertPoint(hit);
        builder.CreateCall(record_printer(record_name), {record, is_inner});
        builder.CreateBr(done_block);
        builder.SetInsertPoint(miss);
      }
      builder.CreateBr(done_block);
      builder.SetInsertPoint(done_block);
      builder.CreateRetVoid();
      current_function = outer_function;
      return printer;
    }
    auto *const entry = llvm::BasicBlock::Create(context, "entry", printer);
    auto *const empty_block = llvm::BasicBlock::Create(context, "empty", printer);
    auto *const record_block =
        llvm::BasicBlock::Create(context, "record", printer);
    auto *const open_block = llvm::BasicBlock::Create(context, "open", printer);
    auto *const fields_block =
        llvm::BasicBlock::Create(context, "fields", printer);
    auto *const end_block = llvm::BasicBlock::Create(context, "end", printer);
    builder.SetInsertPoint(entry);
    builder.CreateCondBr(builder.CreateIsNull(record), empty_block,
                         record_block);
    builder.SetInsertPoint(empty_block);
    print_text("nothing");
    builder.CreateRetVoid();

    builder.SetInsertPoint(record_block);
    builder.CreateCondBr(is_inner, open_block, fields_block);
    builder.SetInsertPoint(open_block);
    print_text("(");
    builder.CreateBr(fields_block);

    builder.SetInsertPoint(fields_block);
    const auto &declaration = *records.at(p_name);
    if (const auto spoken = core::record_print_names().find(p_name);
        spoken != core::record_print_names().end())
      print_text(spoken->second);
    else
      print_text(core::describe_in_english(value_type::record_of(p_name)));
    for (std::size_t index = 0; index < declaration.fields.size(); ++index) {
      const auto &field = declaration.fields[index];
      // A field named in several words is `number_of_wheels` in Ovis Core,
      // and printed as written.
      auto spoken = field.name;
      std::ranges::replace(spoken, '_', ' ');
      print_text((index == 0 ? " with " : ", ") + spoken + " ");
      if (declaration.is_c && field.type.is_record() && !field.inline_record) {
        // A C pointer to another struct: C's lists and trees can loop.
        print_text(core::describe_in_english(field.type));
        continue;
      }
      print_inner(load_field(record, p_name, field), field.type);
    }
    auto *const close_block =
        llvm::BasicBlock::Create(context, "close", printer);
    builder.CreateCondBr(is_inner, close_block, end_block);
    builder.SetInsertPoint(close_block);
    print_text(")");
    builder.CreateBr(end_block);
    builder.SetInsertPoint(end_block);
    builder.CreateRetVoid();
    current_function = outer_function;
    return printer;
  }

  auto print_record_list(llvm::Value *p_list, const value_type &p_type)
      -> void {
    const auto element = item_storage(p_type);
    print_text("[");
    for_range(builder.getInt64(0),
              builder.CreateSub(runtime_call("ovis_list_len",
                                             builder.getInt64Ty(), {p_list}),
                                builder.getInt64(1)),
              [&](llvm::Value *p_index) {
                auto *const comma_block = llvm::BasicBlock::Create(
                    context, "list.comma", current_function);
                auto *const item_block = llvm::BasicBlock::Create(
                    context, "list.item", current_function);
                builder.CreateCondBr(
                    builder.CreateICmpSGT(p_index, builder.getInt64(0)),
                    comma_block, item_block);
                builder.SetInsertPoint(comma_block);
                print_text(", ");
                builder.CreateBr(item_block);
                builder.SetInsertPoint(item_block);
                print_inner(load_element(element_address(p_list, p_index,
                                                         element),
                                         element),
                            item_value(element));
              });
    print_text("]");
  }

  auto print_value(llvm::Value *p_value, const value_type &p_type) -> void {
    if (p_type.is_map()) {
      print_map(p_value, p_type);
      return;
    }
    if (p_type.is_list &&
        (p_type.base == base_type::record || p_type.inner_lists > 0 ||
         !p_type.pointee.empty())) {
      print_record_list(p_value, p_type);
      return;
    }
    if (p_type.is_list) {
      runtime_call("ovis_print_list", builder.getVoidTy(),
                   {p_value, list_kind(p_type)});
      return;
    }
    switch (p_type.base) {
    case base_type::integer:
      if (*p_type.number_type() == core::c_type::size_type)
        runtime_call("ovis_print_uint", builder.getVoidTy(), {p_value});
      else
        runtime_call("ovis_print_int", builder.getVoidTy(),
                     {coerce(p_value, p_type, value_type::integer())});
      break;
    case base_type::real:
      if (p_type.is_sized())
        runtime_call("ovis_print_float32", builder.getVoidTy(), {p_value});
      else
        runtime_call("ovis_print_float", builder.getVoidTy(), {p_value});
      break;
    case base_type::boolean:
      runtime_call("ovis_print_bool", builder.getVoidTy(),
                   {builder.CreateZExt(p_value, builder.getInt32Ty())});
      break;
    case base_type::string:
      runtime_call("ovis_print_str", builder.getVoidTy(), {p_value});
      break;
    case base_type::record:
      builder.CreateCall(record_printer(p_type.record),
                         {p_value, builder.getInt1(false)});
      break;
    case base_type::nothing:
    case base_type::pointer:
    case base_type::memory:
    case base_type::map: // printed above
    case base_type::unknown:
      break;
    }
  }

  // Notes the source line being run, for runtime errors in checked builds.
  auto store_line(std::size_t p_line) -> void {
    if (checked && line_global != nullptr && p_line != 0)
      builder.CreateStore(builder.getInt64(p_line), line_global);
  }

  // After a call: the callee set the line to its own, and this statement is
  // still running. A function without source lines (the prelude's) puts
  // back the line it was called from (`the first item of xs where _ is
  // even` fails on the program's line, not the phrase's).
  auto restore_line() -> void {
    if (current_line != 0)
      store_line(current_line);
    else if (entry_line != nullptr)
      builder.CreateStore(entry_line, line_global);
  }

  auto generate(const statement &p_statement) -> void {
    using kind = statement::kind_type;
    if (checked && current_lines != nullptr &&
        p_statement.location.line < current_lines->size()) {
      current_line = (*current_lines)[p_statement.location.line];
      store_line(current_line);
    }
    const auto &values = p_statement.values;
    switch (p_statement.kind) {
    case kind::let: {
      const auto type = *p_statement.declared_type;
      auto *const value =
          values.empty()
              ? zero_of(type)
              : coerce(generate(values[0]), values[0].type, type);
      if (in_main && scopes.size() == 1) {
        builder.CreateStore(value, globals.at(p_statement.name).address);
      } else {
        builder.CreateStore(value,
                            declare_local(p_statement.name, type).address);
      }
      if (!values.empty())
        name_value(value, type, p_statement.name);
      break;
    }
    case kind::set: {
      const auto &target = lookup(p_statement.name);
      auto *const value =
          coerce(generate(values[0]), values[0].type, target.type);
      builder.CreateStore(value, target.address);
      name_value(value, target.type, p_statement.name);
      break;
    }
    case kind::put: {
      const auto &target = lookup(p_statement.name);
      const auto element = item_storage(target.type);
      auto *const list =
          builder.CreateLoad(builder.getPtrTy(), target.address);
      auto *const address =
          element_address(list, generate(values[0]), element);
      store_element(address,
                    coerce(generate(values[1]), values[1].type, element),
                    element);
      break;
    }
    case kind::print:
      for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0)
          runtime_call("ovis_print_space", builder.getVoidTy(), {});
        print_value(generate(values[index]), values[index].type);
      }
      runtime_call("ovis_print_newline", builder.getVoidTy(), {});
      break;
    case kind::if_: {
      auto *const then_block =
          llvm::BasicBlock::Create(context, "if.then", current_function);
      auto *const else_block =
          llvm::BasicBlock::Create(context, "if.else", current_function);
      auto *const end_block =
          llvm::BasicBlock::Create(context, "if.end", current_function);
      builder.CreateCondBr(generate(values[0]), then_block, else_block);
      builder.SetInsertPoint(then_block);
      generate_block(p_statement.body);
      if (!is_terminated())
        builder.CreateBr(end_block);
      builder.SetInsertPoint(else_block);
      generate_block(p_statement.else_body);
      if (!is_terminated())
        builder.CreateBr(end_block);
      builder.SetInsertPoint(end_block);
      break;
    }
    case kind::while_: {
      auto *const condition_block =
          llvm::BasicBlock::Create(context, "while.condition", current_function);
      auto *const body_block =
          llvm::BasicBlock::Create(context, "while.body", current_function);
      auto *const end_block =
          llvm::BasicBlock::Create(context, "while.end", current_function);
      builder.CreateBr(condition_block);
      builder.SetInsertPoint(condition_block);
      builder.CreateCondBr(generate(values[0]), body_block, end_block);
      builder.SetInsertPoint(body_block);
      generate_loop(condition_block, condition_block, end_block,
                    p_statement.body);
      builder.SetInsertPoint(end_block);
      break;
    }
    case kind::for_:
    case kind::each: {
      const bool is_each = p_statement.kind == kind::each;
      scopes.emplace_back();
      auto *const counter =
          create_entry_alloca(builder.getInt64Ty(), p_statement.name + ".index");
      llvm::Value *list = nullptr;
      llvm::Value *last = nullptr;
      // A map is gone through by its keys, listed when the loop starts (and
      // freed when it ends).
      const bool over_map = is_each && values[0].type.is_map();
      const auto items = over_map ? value_type::list_of(values[0].type.map_key())
                                  : values[0].type;
      if (is_each) {
        list = generate(values[0]);
        if (over_map)
          list = map_keys(list, values[0].type);
        builder.CreateStore(builder.getInt64(0), counter);
      } else {
        builder.CreateStore(generate(values[0]), counter);
        last = generate(values[1]);
      }

      auto *const condition_block =
          llvm::BasicBlock::Create(context, "for.condition", current_function);
      auto *const body_block =
          llvm::BasicBlock::Create(context, "for.body", current_function);
      auto *const step_block =
          llvm::BasicBlock::Create(context, "for.step", current_function);
      auto *const end_block =
          llvm::BasicBlock::Create(context, "for.end", current_function);
      builder.CreateBr(condition_block);

      builder.SetInsertPoint(condition_block);
      auto *const index = builder.CreateLoad(builder.getInt64Ty(), counter);
      auto *const keep_going =
          is_each ? builder.CreateICmpSLT(
                        index, runtime_call("ovis_list_len",
                                            builder.getInt64Ty(), {list}))
                  : p_statement.counts_down ? builder.CreateICmpSGE(index, last)
                                            : builder.CreateICmpSLE(index, last);
      builder.CreateCondBr(keep_going, body_block, end_block);

      builder.SetInsertPoint(body_block);
      if (is_each) {
        const auto element = item_storage(items);
        auto &item = declare_local(p_statement.name, item_value(element));
        builder.CreateStore(
            load_element(element_address(list, index, element), element),
            item.address);
      } else {
        // The loop variable is a copy, so assigning to it in the body does
        // not change how often the loop runs.
        auto &item = declare_local(p_statement.name, value_type::integer());
        builder.CreateStore(index, item.address);
      }
      generate_loop(condition_block, step_block, end_block, p_statement.body);
      auto *const current = builder.CreateLoad(builder.getInt64Ty(), counter);
      builder.CreateStore(p_statement.counts_down
                              ? builder.CreateSub(current, builder.getInt64(1))
                              : builder.CreateAdd(current, builder.getInt64(1)),
                          counter);
      builder.CreateBr(condition_block);
      builder.SetInsertPoint(end_block);
      if (over_map)
        runtime_call("ovis_free_list", builder.getVoidTy(), {list});
      scopes.pop_back();
      break;
    }
    case kind::defer:
      frames.back().push_back({.deferred = &p_statement});
      break;
    case kind::with_memory: {
      // What is made in the body goes in this memory; every way out of the
      // body (falling off, give back, stop, skip) pops it, and frees it
      // when the block made it.
      auto *const memory = values.empty()
                               ? runtime_call("ovis_memory_new",
                                              builder.getPtrTy(), {})
                               : generate(values[0]);
      runtime_call("ovis_memory_push", builder.getVoidTy(), {memory});
      generate_block(p_statement.body,
                     exit_action{.memory = memory,
                                 .frees = values.empty(),
                                 .line = current_line});
      break;
    }
    case kind::block: {
      auto *const end_block =
          llvm::BasicBlock::Create(context, "block.end", current_function);
      bare_blocks.push_back(
          {p_statement.label, end_block, frames.size(),
           p_statement.name.empty() ? nullptr : find_variable(p_statement.name),
           p_statement.name});
      generate_block(p_statement.body);
      bare_blocks.pop_back();
      if (!is_terminated())
        builder.CreateBr(end_block);
      builder.SetInsertPoint(end_block);
      break;
    }
    case kind::leave: {
      const auto target = std::find_if(
          bare_blocks.rbegin(), bare_blocks.rend(), [&](const auto &p_block) {
            return p_statement.label.empty() || p_block.label == p_statement.label;
          });
      // The value is worked out before the deferred statements run, as for
      // `return`.
      if (!values.empty()) {
        const auto &variable = *target->gives;
        auto *const value =
            coerce(generate(values[0]), values[0].type, variable.type);
        builder.CreateStore(value, variable.address);
        name_value(value, variable.type, target->name);
      }
      emit_frames(target->frame_depth);
      builder.CreateBr(target->end_block);
      start_unreachable_block();
      break;
    }
    case kind::break_:
    case kind::continue_:
      emit_frames(loops.back().frame_depth);
      builder.CreateBr(p_statement.kind == kind::break_
                           ? loops.back().break_block
                           : loops.back().continue_block);
      start_unreachable_block();
      break;
    case kind::return_:
      generate_return(values.empty() ? nullptr : &values[0]);
      start_unreachable_block();
      break;
    case kind::evaluate:
      generate(values[0]);
      break;
    case kind::set_field: {
      const auto &record = values[0].type.record;
      const auto &field = *records.at(record)->find(p_statement.name);
      auto *const object = generate(values[0]);
      store_field(object, record, field, generate(values[1]), values[1].type);
      break;
    }
    case kind::pass:
      break;
    }
  }

  // The value is worked out before the deferred statements run, so they
  // can't change what is given back.
  auto generate_return(const expression *p_value) -> void {
    if (in_main) {
      auto *const value =
          p_value == nullptr
              ? builder.getInt32(0)
              : builder.CreateTrunc(generate(*p_value), builder.getInt32Ty());
      emit_frames(0);
      builder.CreateRet(value);
      return;
    }
    if (current_return_type == value_type::nothing()) {
      emit_frames(0);
      builder.CreateRetVoid();
      return;
    }
    auto *const value = p_value == nullptr
                            ? zero_of(current_return_type)
                            : coerce(generate(*p_value), p_value->type,
                                     current_return_type);
    emit_frames(0);
    builder.CreateRet(value);
  }

  auto find_function(const string_type &p_name) const -> const core::function * {
    for (const auto &unit : source.units)
      for (const auto &function : unit.functions)
        if (function.name == p_name)
          return &function;
    return nullptr;
  }

  auto declare_functions() -> void {
    for (const auto &unit : source.units) {
      for (const auto &function : unit.functions) {
        std::vector<llvm::Type *> parameters;
        for (const auto &parameter : function.parameters)
          parameters.push_back(type_of(parameter.type));
        auto *const type = llvm::FunctionType::get(
            type_of(function.return_type), parameters, false);
        // Prefixed so user functions never clash with C library symbols.
        auto *const declaration =
            llvm::Function::Create(type, llvm::Function::InternalLinkage,
                                   "ovis." + function.name, module.get());
        for (std::size_t index = 0; index < parameters.size(); ++index)
          declaration->getArg(index)->setName(function.parameters[index].name);
        functions[function.name] = declaration;
      }
    }
  }

  // One table per trait method, with the function each record answers it
  // with: its impl's, else the trait's default.
  auto declare_dispatch() -> void {
    for (const auto &unit : source.units) {
      for (const auto &trait : unit.traits)
        traits.emplace(trait.name, &trait);
      for (const auto &impl : unit.impls)
        implemented.emplace(impl.trait, impl.type);
    }
    for (const auto &[trait_name, trait] : traits) {
      for (const auto &method : trait->methods) {
        method_owner.emplace(method.name, trait);
        std::vector<llvm::Constant *> entries(
            records.size(),
            llvm::Constant::getNullValue(builder.getPtrTy()));
        std::size_t answered = 0;
        for (const auto &[record_name, record] : records) {
          if (!implemented.contains({trait_name, record_name}))
            continue;
          const auto written =
              functions.find(core::impl_function_name(method.name, record_name));
          if (written != functions.end())
            entries[record_tags.at(record_name)] = written->second;
          else if (method.has_default)
            entries[record_tags.at(record_name)] = functions.at(method.name);
          if (!entries[record_tags.at(record_name)]->isNullValue())
            ++answered;
        }
        if (answered == 1)
          for (std::size_t tag = 0; tag < entries.size(); ++tag)
            if (!entries[tag]->isNullValue())
              single_implementation[method.name] = {
                  tag, llvm::cast<llvm::Function>(entries[tag])};
        auto *const type =
            llvm::ArrayType::get(builder.getPtrTy(), records.size());
        dispatch_tables[method.name] = new llvm::GlobalVariable(
            *module, type, true, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(type, entries),
            "ovis.dispatch." + method.name);
      }
    }
  }

  auto declare_globals() -> void {
    for (const auto &unit : source.units) {
      for (const auto &statement : unit.statements) {
        if (statement.kind != statement::kind_type::let ||
            globals.contains(statement.name))
          continue;
        auto *const type = type_of(*statement.declared_type);
        auto *const global = new llvm::GlobalVariable(
            *module, type, false, llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(type), statement.name);
        globals[statement.name] = {global, *statement.declared_type};
      }
    }
  }

  auto finish_function() -> void {
    if (is_terminated())
      return;
    generate_return(nullptr);
  }

  // Every global a function reads or sets: a name it uses that isn't its
  // own (a parameter, or declared in it).
  auto names_used_in_functions() const -> std::set<string_type> {
    std::set<string_type> result;
    std::set<string_type> used;
    std::set<string_type> own;
    std::function<void(const expression &)> in_expression =
        [&](const expression &p_expression) {
          if (p_expression.kind == expression::kind_type::variable)
            used.insert(p_expression.text);
          for (const auto &argument : p_expression.arguments)
            in_expression(argument);
        };
    std::function<void(const std::vector<statement> &)> in_block =
        [&](const std::vector<statement> &p_block) {
          for (const auto &statement : p_block) {
            using kind = statement::kind_type;
            if (statement.kind == kind::let || statement.kind == kind::each ||
                statement.kind == kind::for_)
              own.insert(statement.name);
            else
              used.insert(statement.name);
            for (const auto &value : statement.values)
              in_expression(value);
            in_block(statement.body);
            in_block(statement.else_body);
          }
        };
    for (const auto &unit : source.units)
      for (const auto &function : unit.functions) {
        used.clear();
        own.clear();
        for (const auto &parameter : function.parameters)
          own.insert(parameter.name);
        in_block(function.body);
        for (const auto &name : used)
          if (!own.contains(name))
            result.insert(name);
      }
    return result;
  }

  auto define_main() -> void {
    auto *const main = llvm::Function::Create(
        llvm::FunctionType::get(builder.getInt32Ty(), false),
        llvm::Function::ExternalLinkage, "main", module.get());
    current_function = main;
    in_main = true;
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", main));
    runtime_call("ovis_init", builder.getVoidTy(), {});
    scopes.emplace_back();
    // Globals whose type has a non-zero default (strings, lists) need it set
    // before any function can observe them. One no function uses is only
    // read after its `let`, which gives it its value: an empty list made
    // for it would never be freed.
    const auto used = names_used_in_functions();
    for (const auto &[name, global] : globals)
      if (global.type == value_type::string() ||
          (global.type.is_list && used.contains(name)))
        builder.CreateStore(zero_of(global.type), global.address);
    frames.emplace_back();
    for (const auto &unit : source.units) {
      current_lines = &unit.source_lines;
      naming = checked && unit.module != "prelude";
      for (const auto &statement : unit.statements)
        generate(statement);
    }
    current_lines = nullptr;
    finish_function();
    frames.pop_back();
    scopes.pop_back();
    in_main = false;
  }

  auto define_functions() -> void {
    for (const auto &unit : source.units) {
      current_lines = &unit.source_lines;
      naming = checked && unit.module != "prelude";
      for (const auto &function : unit.functions) {
        current_line = 0;
        current_function = functions.at(function.name);
        current_return_type = function.return_type;
        builder.SetInsertPoint(
            llvm::BasicBlock::Create(context, "entry", current_function));
        entry_line = checked && line_global != nullptr && unit.source_lines.empty()
                         ? builder.CreateLoad(builder.getInt64Ty(), line_global)
                         : nullptr;
        scopes.emplace_back();
        for (std::size_t index = 0; index < function.parameters.size();
             ++index) {
          const auto &parameter = function.parameters[index];
          builder.CreateStore(current_function->getArg(index),
                              declare_local(parameter.name, parameter.type)
                                  .address);
        }
        frames.emplace_back();
        for (const auto &statement : function.body)
          generate(statement);
        finish_function();
        entry_line = nullptr;
        frames.pop_back();
        scopes.pop_back();
      }
    }
  }

public:
  generator(const core::program &p_program, llvm::LLVMContext &p_context,
            string_view_type p_module_name, bool p_checked)
      : source(p_program), context(p_context),
        module(std::make_unique<llvm::Module>(
            llvm::StringRef(p_module_name.data(), p_module_name.size()),
            p_context)),
        builder(p_context), checked(p_checked) {
    // Sizes worked out while generating (a record's, `size of`) are the
    // target's: a record's fields lie as a C struct's would.
    set_target(*module, *host_target_machine());
    for (const auto &function : core::libc_functions())
      c_functions[function.name] = &function;
    for (const auto &unit : p_program.units)
      for (const auto &function : unit.externs)
        c_functions[function.name] = &function;
  }

  auto declare_records() -> void {
    for (const auto &unit : source.units)
      for (const auto &record : unit.records)
        records[record.name] = &record;
    for (const auto &[name, record] : records) {
      record_tags[name] = static_cast<unsigned>(record_tags.size());
      if (record->is_c) {
        record_types[name] = llvm::StructType::create(
            context,
            {llvm::ArrayType::get(builder.getInt8Ty(),
                                  std::max<std::uint64_t>(record->size, 1))},
            "ovis.c." + name);
        continue;
      }
      std::vector<llvm::Type *> fields;
      if (record->has_tag())
        fields.push_back(builder.getInt64Ty());
      for (const auto &field : record->fields)
        fields.push_back(type_of(field.type));
      record_types[name] = llvm::StructType::create(
          context, fields, "ovis.record." + name, record->is_packed);
    }
  }

  auto run() -> std::unique_ptr<llvm::Module> {
    if (checked)
      line_global = new llvm::GlobalVariable(
          *module, builder.getInt64Ty(), false,
          llvm::GlobalValue::ExternalLinkage, nullptr, "ovis_line");
    // The constants' section must exist for the runtime to look for it.
    {
      auto *const marker = constant_text_global("");
      marker->setName("ovis_const_marker");
    }
    declare_records();
    declare_functions();
    declare_dispatch();
    declare_globals();
    define_main();
    define_functions();

    string_type problems;
    llvm::raw_string_ostream stream(problems);
    if (llvm::verifyModule(*module, &stream))
      throw error("Internal compiler error, generated invalid LLVM IR:\n" +
                  problems);
    return std::move(module);
  }
};

auto host_target_machine() -> std::unique_ptr<llvm::TargetMachine> {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  // LLVM 21 takes a Triple where LLVM 18 took a string.
#if LLVM_VERSION_MAJOR >= 21
  const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
#else
  const auto triple = llvm::sys::getDefaultTargetTriple();
#endif
  string_type problem;
  const auto *const target = llvm::TargetRegistry::lookupTarget(triple, problem);
  if (target == nullptr)
    throw error("No LLVM target for " + llvm::Triple(triple).str() + ": " +
                problem);
  return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      triple, "generic", "", llvm::TargetOptions(), llvm::Reloc::PIC_));
}

auto set_target(llvm::Module &p_module, const llvm::TargetMachine &p_machine)
    -> void {
#if LLVM_VERSION_MAJOR >= 21
  p_module.setTargetTriple(p_machine.getTargetTriple());
#else
  p_module.setTargetTriple(p_machine.getTargetTriple().str());
#endif
  p_module.setDataLayout(p_machine.createDataLayout());
}

auto find_c_compiler() -> string_type {
  for (const auto *const name : {"cc", "clang", "gcc"})
    if (auto found = llvm::sys::findProgramByName(name))
      return *found;
  throw error("A C compiler (cc, clang or gcc) is needed to link programs.");
}

} // namespace

auto generate_module(const core::program &p_program,
                     llvm::LLVMContext &p_context,
                     string_view_type p_module_name, bool p_checked)
    -> std::unique_ptr<llvm::Module> {
  return generator(p_program, p_context, p_module_name, p_checked).run();
}

auto optimize(llvm::Module &p_module) -> void {
  const auto machine = host_target_machine();
  set_target(p_module, *machine);

  llvm::LoopAnalysisManager loop_analysis;
  llvm::FunctionAnalysisManager function_analysis;
  llvm::CGSCCAnalysisManager cgscc_analysis;
  llvm::ModuleAnalysisManager module_analysis;
  llvm::PassBuilder passes(machine.get());
  passes.registerModuleAnalyses(module_analysis);
  passes.registerCGSCCAnalyses(cgscc_analysis);
  passes.registerFunctionAnalyses(function_analysis);
  passes.registerLoopAnalyses(loop_analysis);
  passes.crossRegisterProxies(loop_analysis, function_analysis, cgscc_analysis,
                              module_analysis);
  passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2)
      .run(p_module, module_analysis);
}

auto to_ir_text(const llvm::Module &p_module) -> string_type {
  string_type result;
  llvm::raw_string_ostream stream(result);
  p_module.print(stream, nullptr);
  return result;
}

auto libraries_of(const core::program &p_program) -> std::vector<string_type> {
  std::vector<string_type> result = {"-lm"};
  const auto add = [&](string_type p_flag) {
    if (std::ranges::find(result, p_flag) == result.end())
      result.push_back(std::move(p_flag));
  };
  for (const auto &unit : p_program.units) {
    for (const auto &flag : unit.link_flags)
      add(flag);
    for (const auto &library : unit.links)
      add("-l" + library);
  }
  return result;
}

auto c_wrappers_of(const core::program &p_program) -> c_wrappers {
  c_wrappers result;
  std::set<string_type> written;
  std::set<string_type> headers;
  string_type functions;
  // A generic C macro's copies are externs of their own.
  std::vector<const core::c_function *> wrapped;
  for (const auto &unit : p_program.units)
    for (const auto &function : unit.externs)
      wrapped.push_back(&function);
  for (const auto *const each : wrapped) {
    const auto &function = *each;
    if (!function.needs_wrapper() || !written.insert(function.name).second)
      continue;
    // A macro's copy's arguments' structs may be another header's.
    for (const auto &header : function.macro_headers)
      if (headers.insert(header).second)
        result.source += "#include \"" + header + "\"\n";
    if (headers.insert(function.header_path).second) {
      result.source += "#include \"" + function.header_path + "\"\n";
      for (const auto &flag : function.compile_flags)
        if (std::ranges::find(result.flags, flag) == result.flags.end())
          result.flags.push_back(flag);
    }
    // Pointers travel as `void *` and are cast back with the header's
    // spelling, which also reads for function pointers.
    string_type parameters;
    string_type arguments;
    std::vector<string_type> values;
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      const auto name = "p" + std::to_string(index);
      const auto &spelling = function.spellings[index];
      const bool by_value = function.by_value[index];
      const bool pointer =
          !by_value && (function.parameters[index] == core::c_type::pointer_type ||
                        function.parameters[index] == core::c_type::string_type);
      parameters += string_type(index > 0 ? ", " : "") +
                    (by_value  ? spelling + " *" + name
                     : pointer ? "void *" + name
                               : spelling + " " + name);
      values.push_back(by_value  ? "*" + name
                       : pointer ? "(" + spelling + ")" + name
                                 : name);
      arguments += string_type(index > 0 ? ", " : "") + values.back();
    }
    if (parameters.empty())
      parameters = "void";
    // A macro's copy gives each of the macro's parameters a value, an
    // address's value, a type or a field.
    if (!function.macro_arguments.empty()) {
      arguments.clear();
      for (const auto &each : function.macro_arguments) {
        string_type value = each;
        if (each.size() > 1 && each[0] == 'p' &&
            std::isdigit(static_cast<unsigned char>(each[1])))
          value = values.at(std::stoul(each.substr(1)));
        else if (each.starts_with("(*p"))
          value = "(*" + values.at(std::stoul(each.substr(3))) + ")";
        arguments += (arguments.empty() ? "" : ", ") + value;
      }
    }
    const auto call =
        (function.macro_name.empty() ? function.name : function.macro_name) +
        "(" + arguments + ")";
    if (function.result_by_value)
      functions += "void *ovis_w_" + function.name + "(" + parameters +
                   ") {\n  " + function.result_spelling +
                   " *result = ovis_record_new(sizeof *result);\n  *result = " +
                   call + ";\n  return result;\n}\n";
    else if (function.result == core::c_type::void_type)
      functions += "void ovis_w_" + function.name + "(" + parameters +
                   ") { " + call + "; }\n";
    else if (function.result == core::c_type::pointer_type ||
             function.result == core::c_type::string_type)
      functions += "void *ovis_w_" + function.name + "(" + parameters +
                   ") { return (void *)" + call + "; }\n";
    else
      functions += function.result_spelling + " ovis_w_" + function.name +
                   "(" + parameters + ") { return " + call + "; }\n";
  }
  // A macro's copy spells its types as C's fixed sizes (`uint32_t`).
  if (!functions.empty())
    result.source = "#include <stdint.h>\n#include <stddef.h>\n" + result.source +
                    "extern void *ovis_record_new(long long size);\n" + functions;
  return result;
}

namespace {

// Compiles the wrappers to an object file.
auto compile_wrappers(const c_wrappers &p_wrappers,
                      const std::filesystem::path &p_directory)
    -> std::filesystem::path {
  const auto source = p_directory / "ovis_wrappers.c";
  const auto object = p_directory / "ovis_wrappers.o";
  std::ofstream(source) << p_wrappers.source;
  const auto compiler = find_c_compiler();
  const auto source_text = source.string();
  const auto object_text = object.string();
  std::vector<llvm::StringRef> arguments = {compiler, "-O2", "-w", "-c",
                                            source_text, "-o", object_text};
  for (const auto &flag : p_wrappers.flags)
    arguments.push_back(flag);
  string_type message;
  if (llvm::sys::ExecuteAndWait(compiler, arguments, std::nullopt, {}, 0, 0,
                                &message) != 0)
    throw error("Compiling the C wrappers for structs passed by value failed (" +
                compiler + "): " + message);
  return object;
}

} // namespace

auto emit_object(llvm::Module &p_module, const std::filesystem::path &p_output,
                 const c_wrappers &p_wrappers) -> void {
  if (p_wrappers.source.empty()) {
    emit_object(p_module, p_output);
    return;
  }
  // One object for the build system: the program's and the wrappers',
  // joined by a partial link.
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("ovis-object-" + std::to_string(llvm::sys::Process::getProcessId()));
  std::filesystem::create_directories(directory);
  const auto program = directory / "program.o";
  emit_object(p_module, program);
  const auto wrappers = compile_wrappers(p_wrappers, directory);
  const auto compiler = find_c_compiler();
  const auto program_text = program.string();
  const auto wrappers_text = wrappers.string();
  const auto output_text = p_output.string();
  std::vector<llvm::StringRef> arguments = {compiler,      "-r",
                                            program_text,  wrappers_text,
                                            "-o",          output_text};
  string_type message;
  const auto status = llvm::sys::ExecuteAndWait(compiler, arguments, std::nullopt,
                                                {}, 0, 0, &message);
  std::filesystem::remove_all(directory);
  if (status != 0)
    throw error("Joining the program's object with its C wrappers failed (" +
                compiler + "): " + message);
}

auto emit_object(llvm::Module &p_module, const std::filesystem::path &p_output)
    -> void {
  const auto machine = host_target_machine();
  set_target(p_module, *machine);
  std::error_code code;
  llvm::raw_fd_ostream object(p_output.string(), code, llvm::sys::fs::OF_None);
  if (code)
    throw error("Cannot write " + p_output.string() + ": " + code.message());
  llvm::legacy::PassManager emitter;
  if (machine->addPassesToEmitFile(emitter, object, nullptr,
                                   llvm::CodeGenFileType::ObjectFile))
    throw error("LLVM cannot emit object files for this target.");
  emitter.run(p_module);
}

auto build_executable(llvm::Module &p_module,
                      const std::filesystem::path &p_output,
                      const std::vector<string_type> &p_link_flags,
                      bool p_checked, bool p_report_leaks,
                      const c_wrappers &p_wrappers) -> void {
  const auto work_directory =
      std::filesystem::temp_directory_path() /
      ("ovis-" + std::to_string(llvm::sys::Process::getProcessId()));
  std::filesystem::create_directories(work_directory);
  const auto object_path = work_directory / "program.o";
  const auto runtime_path = work_directory / "runtime.c";

  emit_object(p_module, object_path);
  std::ofstream(runtime_path) << runtime_source();

  const auto compiler = find_c_compiler();
  const auto output = p_output.string();
  const auto object = object_path.string();
  const auto runtime = runtime_path.string();
  std::vector<llvm::StringRef> arguments = {compiler, "-O2", "-w",  object,
                                            runtime,  "-o",  output};
  if (p_checked)
    arguments.push_back("-DOVIS_CHECKED");
  if (p_checked && p_report_leaks)
    arguments.push_back("-DOVIS_REPORT_LEAKS");
  // The wrappers call the libraries, so they come before their flags.
  string_type wrappers_text;
  if (!p_wrappers.source.empty()) {
    wrappers_text = compile_wrappers(p_wrappers, work_directory).string();
    arguments.push_back(wrappers_text);
  }
  for (const auto &flag : p_link_flags)
    arguments.push_back(flag);
  string_type message;
  const auto status =
      llvm::sys::ExecuteAndWait(compiler, arguments, std::nullopt, {}, 0, 0,
                                &message);
  std::filesystem::remove_all(work_directory);
  if (status != 0)
    throw error("Linking failed (" + compiler + "): " + message);
}

} // namespace ovis::codegen
