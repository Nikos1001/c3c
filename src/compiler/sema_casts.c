// Copyright (c) 2019 Christoffer Lerno. All rights reserved.
// Use of this source code is governed by the GNU LGPLv3.0 license
// a copy of which can be found in the LICENSE file.

#include "sema_internal.h"

#pragma clang diagnostic push
#pragma ide diagnostic ignored "ConstantFunctionResult"

static bool bitstruct_cast(Expr *expr, Type *from_type, Type *to, Type *to_type);
static void sema_error_const_int_out_of_range(Expr *expr, Expr *problem, Type *to_type);
static Expr *recursive_may_narrow_float(Expr *expr, Type *type);
static Expr *recursive_may_narrow_int(Expr *expr, Type *type);
static void recursively_rewrite_untyped_list(Expr *expr, Expr **list);
static inline bool cast_may_implicit_ptr(Type *from_pointee, Type *to_pointee);
static inline bool cast_may_array(Type *from, Type *to, bool is_explicit);

static inline bool insert_cast(Expr *expr, CastKind kind, Type *type)
{
	assert(expr->resolve_status == RESOLVE_DONE);
	assert(expr->type);
	Expr *inner = expr_copy(expr);
	expr->expr_kind = EXPR_CAST;
	expr->cast_expr.kind = kind;
	expr->cast_expr.expr = exprid(inner);
	expr->cast_expr.type_info = 0;
	expr->type = type;
	return true;
}

bool sema_error_failed_cast(Expr *expr, Type *from, Type *to)
{
	SEMA_ERROR(expr, "The cast %s to %s is not allowed.", type_quoted_error_string(from), type_quoted_error_string(to));
	return false;
}

Type *cast_infer_len(Type *to_infer, Type *actual_type)
{
	Type *may_infer = to_infer->canonical;
	Type *actual = actual_type->canonical;
	if (may_infer == actual) return to_infer;
	bool canonical_same_kind = may_infer->type_kind == to_infer->type_kind;
	if (may_infer->type_kind == TYPE_INFERRED_ARRAY)
	{
		assert(actual_type->type_kind == TYPE_ARRAY);
		Type *base_type = cast_infer_len(canonical_same_kind ? to_infer->array.base :
		                                 may_infer->array.base, actual->array.base);
		return type_get_array(base_type, actual->array.len);
	}
	if (may_infer->type_kind == TYPE_INFERRED_VECTOR)
	{
		assert(actual_type->type_kind == TYPE_VECTOR || actual->type_kind == TYPE_SCALED_VECTOR);
		Type *base_type = cast_infer_len(canonical_same_kind ? to_infer->array.base : may_infer->array.base, actual->array.base);
		if (actual_type->type_kind == TYPE_SCALED_VECTOR)
		{
			return type_get_scaled_vector(base_type);
		}
		return type_get_vector(base_type, actual->array.len);
	}
	if (may_infer->type_kind == TYPE_POINTER)
	{
		assert(actual->type_kind == TYPE_POINTER);
		Type *base_type = cast_infer_len(canonical_same_kind ? to_infer->array.base : may_infer->pointer, actual->pointer);
		return type_get_ptr(base_type);
	}
	UNREACHABLE
}

static inline bool insert_runtime_cast_unless_const(Expr *expr, CastKind kind, Type *type)
{
	if (expr->expr_kind == EXPR_CONST) return false;
	return insert_cast(expr, kind, type);
}


bool pointer_to_integer(Expr *expr, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_PTRXI, type)) return true;

	// Revisit this to support pointers > 64 bits.
	expr_rewrite_const_int(expr, type, expr->const_expr.ptr, false);
	return true;
}

bool pointer_to_bool(Expr *expr, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_PTRBOOL, type)) return true;

	if (expr->const_expr.const_kind == CONST_POINTER)
	{
		expr_rewrite_const_bool(expr, type, expr->const_expr.ptr != 0);
		return true;
	}
	assert(expr->const_expr.const_kind == CONST_STRING);
	expr_rewrite_const_bool(expr, type, true);
	return true;
}


bool pointer_to_pointer(Expr* expr, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_PTRPTR, type)) return true;

	if (expr->const_expr.const_kind == CONST_STRING)
	{
		return insert_cast(expr, CAST_PTRPTR, type);
	}
	// Must have been a null
	expr->type = type;
	expr->const_expr.narrowable = false;
	expr->const_expr.is_hex = false;
	return true;
}


static void const_int_to_fp_cast(Expr *expr, Type *canonical, Type *type)
{
	Real f = int_to_real(expr->const_expr.ixx);
	switch (canonical->type_kind)
	{
		case TYPE_F32:
			expr->const_expr.fxx = (Float) { (float)f, TYPE_F32 };
			break;
		case TYPE_F64:
			expr->const_expr.fxx = (Float) { (double)f, TYPE_F64 };
			break;
		default:
			expr->const_expr.fxx = (Float) { f, canonical->type_kind };
			break;
	}
	expr->type = type;
	expr->const_expr.const_kind = CONST_FLOAT;
	expr->const_expr.narrowable = false;
	expr->const_expr.is_hex = false;
}


/**
 * Bool into a signed or unsigned int.
 */
bool bool_to_int(Expr *expr, Type *canonical, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_BOOLINT, type)) return true;
	expr_rewrite_const_int(expr, type, expr->const_expr.b ? 1 : 0, false);
	return true;
}


/**
 * Cast bool to float.
 */
bool bool_to_float(Expr *expr, Type *canonical, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_BOOLFP, type)) return true;

	assert(expr->const_expr.const_kind == CONST_BOOL);
	expr_rewrite_const_float(expr, type, expr->const_expr.b ? 1.0 : 0.0);
	return true;
}

/**
 * Cast bool to float.
 */
bool voidfail_to_error(Expr *expr, Type *type)
{
	Expr *inner = expr_copy(expr);
	expr->expr_kind = EXPR_CATCH;
	expr->inner_expr = inner;
	expr->type = type;
	return true;
}

/**
 * Convert from any into to bool.
 * @return true for any implicit conversion except assign and assign add.
 */
bool integer_to_bool(Expr *expr, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_INTBOOL, type)) return true;

	expr_rewrite_const_bool(expr, type, !int_is_zero(expr->const_expr.ixx));
	return true;
}

/**
 * Convert from any float to bool
 */
bool float_to_bool(Expr *expr, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_FPBOOL, type)) return true;

	expr_rewrite_const_bool(expr, type, expr->const_expr.fxx.f != 0.0);
	return true;
}


/**
 * Convert from any fp to fp
 */
static bool float_to_float(Expr* expr, Type *canonical, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_FPFP, type)) return true;
	expr_rewrite_const_float(expr, type, expr->const_expr.fxx.f);
	return true;
}

/**
 * Convert from any floating point to int
 */
bool float_to_integer(Expr *expr, Type *canonical, Type *type)
{
	bool is_signed = type_is_signed(canonical);
	if (insert_runtime_cast_unless_const(expr, is_signed ? CAST_FPSI : CAST_FPUI, type)) return true;

	assert(type_is_integer(canonical));
	Real d = expr->const_expr.fxx.f;
	expr->const_expr.ixx = int_from_real(d, canonical->type_kind);
	expr->const_expr.const_kind = CONST_INTEGER;
	expr->type = type;
	expr->const_expr.narrowable = false;
	expr->const_expr.is_hex = false;
	return true;
}


/**
 * Convert from compile time int to any signed or unsigned int
 * @return true unless the conversion was lossy.
 */
static bool int_literal_to_int(Expr *expr, Type *canonical, Type *type)
{
	if (expr->expr_kind != EXPR_CONST)
	{
		SEMA_ERROR(expr, "This expression could not be resolved to a concrete type. Please add more type annotations.");
		UNREACHABLE
	}
	expr->const_expr.ixx = int_conv(expr->const_expr.ixx, canonical->type_kind);
	assert(expr->const_expr.const_kind == CONST_INTEGER);
	expr->type = type;
	expr->const_expr.narrowable = false;
	expr->const_expr.is_hex = false;
	return true;
}

/**
 * Convert from compile time int to any enum
 */
bool integer_to_enum(Expr *expr, Type *canonical, Type *type)
{
	assert(canonical->type_kind == TYPE_ENUM);
	Decl *enum_decl = canonical->decl;
	if (expr->expr_kind != EXPR_CONST)
	{
		Type *underlying_type = enum_decl->enums.type_info->type->canonical;
		if (!cast(expr, underlying_type)) return false;
		return insert_cast(expr, CAST_INTENUM, type);
	}
	unsigned max_enums = vec_size(enum_decl->enums.values);
	Int to_convert = expr->const_expr.ixx;
	if (int_is_neg(to_convert))
	{
		SEMA_ERROR(expr, "A negative number cannot be converted to an enum.");
		return false;
	}
	Int max = { .i.low = max_enums, .type = TYPE_I32 };
	if (int_comp(to_convert, max, BINARYOP_GE))
	{
		SEMA_ERROR(expr, "This value exceeds the number of enums in %s.", canonical->decl->name);
		return false;
	}
	Decl *decl = enum_decl->enums.values[to_convert.i.low];
	expr->const_expr = (ExprConst) {
		.enum_err_val = decl,
		.const_kind = CONST_ENUM
	};
	expr->type = type;
	return true;
}


static bool int_conversion(Expr *expr, CastKind kind, Type *canonical, Type *type)
{
	// Fold pointer casts if narrowing
	if (expr->expr_kind == EXPR_CAST && expr->cast_expr.kind == CAST_PTRXI
	    && type_size(type) <= type_size(expr->type))
	{
		expr->type = type;
		return true;
	}
	if (insert_runtime_cast_unless_const(expr, kind, type)) return true;

	expr->const_expr.ixx = int_conv(expr->const_expr.ixx, canonical->type_kind);
	expr->const_expr.const_kind = CONST_INTEGER;
	expr->type = type;
	expr->const_expr.narrowable = false;
	expr->const_expr.is_hex = false;
	return true;
}


/**
 * Cast a signed or unsigned integer -> floating point
 */
static bool int_to_float(Expr *expr, CastKind kind, Type *canonical, Type *type)
{
	if (insert_runtime_cast_unless_const(expr, kind, type)) return true;
	const_int_to_fp_cast(expr, canonical, type);
	return true;
}


/**
 * Convert a compile time into to a boolean.
 */
static bool int_literal_to_bool(Expr *expr, Type *type)
{
	assert(expr->expr_kind == EXPR_CONST);
	expr_rewrite_const_bool(expr, type, !int_is_zero(expr->const_expr.ixx));
	return true;
}


/**
 * Cast any int to a pointer -> pointer.
 */
static bool int_to_pointer(Expr *expr, Type *type)
{
	assert(type_bit_size(type_uptr) <= 64 && "For > 64 bit pointers, this code needs updating.");
	if (expr->expr_kind == EXPR_CONST)
	{
		if (!int_fits(expr->const_expr.ixx, type_uptr->canonical->type_kind))
		{
			SEMA_ERROR(expr, "'0x%s' does not fit in a pointer.", int_to_str(expr->const_expr.ixx, 16));
			return false;
		}
		expr->const_expr.ptr = expr->const_expr.ixx.i.low;
		expr->type = type;
		expr->const_expr.const_kind = CONST_POINTER;
		return true;
	}
	cast(expr, type_uptr);
	return insert_cast(expr, CAST_XIPTR, type);
}


static bool int_to_int(Expr *left, Type *from_canonical, Type *canonical, Type *type)
{
	assert(from_canonical->canonical == from_canonical);
	switch (from_canonical->type_kind)
	{
		case ALL_SIGNED_INTS:
			return int_conversion(left, type_is_unsigned(canonical) ? CAST_SIUI : CAST_SISI, canonical, type);
		case ALL_UNSIGNED_INTS:
			return int_conversion(left, type_is_unsigned(canonical) ? CAST_UIUI : CAST_UISI, canonical, type);
		default:
			UNREACHABLE
	}
}

static Type *enum_to_int_cast(Expr* expr, Type *from)
{
	assert(from->type_kind == TYPE_ENUM);
	Type *original = from->decl->enums.type_info->type;
	expr->type = original;
	if (expr->expr_kind == EXPR_CONST && expr->const_expr.const_kind == CONST_ENUM)
	{
		expr_rewrite_const_int(expr, original, expr->const_expr.enum_err_val->enum_constant.ordinal, false);
		return original;
	}
	insert_cast(expr, CAST_ENUMLOW, type_add_optional(original, IS_OPTIONAL(expr)));
	return original;
}

static bool enum_to_integer(Expr* expr, Type *from, Type *canonical, Type *type)
{
	Type *result = enum_to_int_cast(expr, from);
	return int_to_int(expr, result->canonical, canonical, type);
}

static bool enum_to_float(Expr* expr, Type *from, Type *canonical, Type *type)
{
	Type *result = enum_to_int_cast(expr, from);
	return int_to_float(expr, type_is_unsigned(result->canonical) ? CAST_UIFP : CAST_SIFP, canonical, type);
}

bool enum_to_bool(Expr* expr, Type *from, Type *type)
{
	enum_to_int_cast(expr, from);
	return integer_to_bool(expr, type);
}

bool enum_to_pointer(Expr* expr, Type *from, Type *type)
{
	enum_to_int_cast(expr, from);
	return int_to_pointer(expr, type);
}


CastKind cast_to_bool_kind(Type *type)
{
	switch (type_flatten(type)->type_kind)
	{
		case TYPE_TYPEDEF:
		case TYPE_DISTINCT:
		case TYPE_INFERRED_ARRAY:
			UNREACHABLE
		case TYPE_BOOL:
			return CAST_BOOLBOOL;
		case TYPE_ANYERR:
			return CAST_EUBOOL;
		case TYPE_SUBARRAY:
			return CAST_SABOOL;
		case ALL_INTS:
			return CAST_INTBOOL;
		case ALL_FLOATS:
			return CAST_FPBOOL;
		case TYPE_POINTER:
			return CAST_PTRBOOL;
		case TYPE_FAULTTYPE:
			return CAST_ERBOOL;
		case TYPE_POISONED:
		case TYPE_VOID:
		case TYPE_STRUCT:
		case TYPE_UNION:
		case TYPE_ENUM:
		case TYPE_FUNC:
		case TYPE_ARRAY:
		case TYPE_TYPEID:
		case TYPE_TYPEINFO:
		case TYPE_VECTOR:
		case TYPE_BITSTRUCT:
		case TYPE_UNTYPED_LIST:
		case TYPE_OPTIONAL:
		case TYPE_ANY:
		case TYPE_FAILABLE_ANY:
		case TYPE_FLEXIBLE_ARRAY:
		case TYPE_SCALED_VECTOR:
		case TYPE_INFERRED_VECTOR:
		case TYPE_MEMBER:
			return CAST_ERROR;
	}
	UNREACHABLE
}


bool cast_may_explicit(Type *from_type, Type *to_type, bool ignore_failability, bool is_const)
{
	// 1. failable -> non-failable can't be cast unless we ignore failability.
	// *or* we're converting a void! to an error code
	if (type_is_optional(from_type) && !type_is_optional(to_type))
	{
		if (from_type->failable == type_void || !from_type->failable)
		{
			// void! x; anyerr y = (anyerr)(x);
			if (to_type->type_kind == TYPE_FAULTTYPE || to_type->type_kind == TYPE_ANYERR) return true;
		}
		if (!ignore_failability) return false;
	}

	// 2. Remove failability and flatten distinct
	from_type = type_no_optional(from_type);
	to_type = type_no_optional(to_type);

	// 3. We flatten the distinct types, since they should be freely convertible
	from_type = type_flatten_distinct_optional(from_type);
	to_type = type_flatten_distinct_optional(to_type);

	// 2. Same underlying type, always ok
	if (from_type == to_type) return true;

	if (type_is_any_arraylike(to_type) && type_is_any_arraylike(from_type))
	{
		return cast_may_array(from_type, to_type, true);
	}

	TypeKind to_kind = to_type->type_kind;
	switch (from_type->type_kind)
	{
		case TYPE_FAILABLE_ANY:
			return true;
		case TYPE_DISTINCT:
		case TYPE_TYPEDEF:
		case TYPE_OPTIONAL:
			UNREACHABLE
		case TYPE_POISONED:
		case TYPE_INFERRED_ARRAY:
		case TYPE_VOID:
		case TYPE_TYPEINFO:
		case TYPE_FUNC:
		case TYPE_FLEXIBLE_ARRAY:
		case TYPE_INFERRED_VECTOR:
		case TYPE_SCALED_VECTOR:
		case TYPE_MEMBER:
			return false;
		case TYPE_TYPEID:
			// May convert to anything pointer sized or larger, no enums
			return type_is_pointer_sized_or_more(to_type);
		case TYPE_BOOL:
			// May convert to any integer / distinct integer / float, no enums
			return type_is_integer(to_type) || type_is_float(to_type);
		case TYPE_BITSTRUCT:
			// A bitstruct can convert to:
			// 1. An int of the same length
			// 2. An integer array of the same length
			if (type_size(to_type) != type_size(from_type)) return false;
			if (type_is_integer(to_type)) return true;
			return to_type->type_kind == TYPE_ARRAY && type_is_integer(to_type->array.base);
		case TYPE_ANYERR:
			// May convert to a bool, an error type or an integer
			return to_type == type_bool || to_kind == TYPE_FAULTTYPE || type_is_integer(to_type);
		case ALL_SIGNED_INTS:
		case ALL_UNSIGNED_INTS:
			// We don't have to match pointer size if it's a constant.
			if (to_kind == TYPE_POINTER && is_const) return true;
			if (to_kind == TYPE_POINTER && type_is_pointer_sized(from_type)) return true;
			if (to_kind == TYPE_ENUM) return true;
			FALLTHROUGH;
		case TYPE_ENUM:
			// Allow conversion int/enum -> float/bool/int
			if (type_is_integer(to_type) || type_is_float(to_type) || to_type == type_bool) return true;
			return false;
		case ALL_FLOATS:
			// Allow conversion float -> float/int/bool/enum
			return type_is_integer(to_type) || type_is_float(to_type) || to_type == type_bool || to_kind == TYPE_ENUM;
		case TYPE_POINTER:
			// Allow conversion ptr -> int (min pointer size)/bool/pointer/vararray
			if ((type_is_integer(to_type) && type_size(to_type) >= type_size(type_iptr)) || to_type == type_bool || to_kind == TYPE_POINTER) return true;
			// Special subarray conversion: someType[N]* -> someType[]
			if (to_kind == TYPE_SUBARRAY && from_type->pointer->type_kind == TYPE_ARRAY && from_type->pointer->array.base == to_type->array.base) return true;
			// Special function pointer conversion
			return false;
		case TYPE_ANY:
			return to_kind == TYPE_POINTER;
		case TYPE_FAULTTYPE:
			// Allow MyError.A -> error, to an integer or to bool
			return to_type->type_kind == TYPE_ANYERR || type_is_integer(to_type) || to_type == type_bool;
		case TYPE_ARRAY:
			if (to_kind == TYPE_VECTOR)
			{
				return to_type->array.len == from_type->array.len && to_type->array.base == from_type->array.base;
			}
			FALLTHROUGH;
		case TYPE_STRUCT:
			if (type_is_substruct(from_type))
			{
				if (cast_may_explicit(from_type->decl->strukt.members[0]->type, to_type, false, false)) return true;
			}
			FALLTHROUGH;
		case TYPE_UNION:
			return type_is_structurally_equivalent(from_type, to_type);
		case TYPE_SUBARRAY:
			if (to_kind == TYPE_SUBARRAY && type_is_pointer(to_type->array.base)
				&& type_is_pointer(from_type->array.base)) return true;
			return to_kind == TYPE_POINTER;
		case TYPE_VECTOR:
			return type_is_structurally_equivalent(type_get_array(from_type->array.base, (uint32_t)from_type->array.len), to_type);
		case TYPE_UNTYPED_LIST:
			REMINDER("Look at untyped list explicit conversions");
			return false;
	}
	UNREACHABLE
}

bool type_may_convert_to_anyerr(Type *type)
{
	if (type_is_optional_any(type)) return true;
	if (!type_is_optional_type(type)) return false;
	return type->failable->canonical == type_void;
}


static inline bool cast_may_array(Type *from, Type *to, bool is_explicit)
{
	RETRY:;
	assert(!type_is_optional(from) && !type_is_optional(to) && "Optional should already been handled");

	bool compare_len = true;
	if (from->type_kind != to->type_kind)
	{
		switch (to->type_kind)
		{
			case TYPE_INFERRED_ARRAY:
				switch (from->type_kind)
				{
					case TYPE_INFERRED_VECTOR:
					case TYPE_VECTOR:
						if (!is_explicit) return false;
						FALLTHROUGH;
					case TYPE_ARRAY:
						compare_len = false;
						break;
					default:
						return false;
				}
				break;
			case TYPE_ARRAY:
				switch (from->type_kind)
				{
					case TYPE_INFERRED_VECTOR:
						compare_len = false;
						FALLTHROUGH;
					case TYPE_VECTOR:
						if (!is_explicit) return false;
						break;
					case TYPE_INFERRED_ARRAY:
						compare_len = false;
						break;
					default:
						return false;
				}
				break;
			case TYPE_INFERRED_VECTOR:
				switch (from->type_kind)
				{
					case TYPE_INFERRED_ARRAY:
					case TYPE_ARRAY:
						if (!is_explicit) return false;
						FALLTHROUGH;
					case TYPE_VECTOR:
					case TYPE_SCALED_VECTOR:
						compare_len = false;
						break;
					default:
						return false;
				}
				break;
			case TYPE_VECTOR:
				switch (from->type_kind)
				{
					case TYPE_INFERRED_ARRAY:
						compare_len = false;
						FALLTHROUGH;
					case TYPE_ARRAY:
						if (!is_explicit) return false;
						break;
					case TYPE_INFERRED_VECTOR:
						compare_len = false;
						break;
					default:
						return false;
				}
				break;
			case TYPE_SCALED_VECTOR:
				if (from->type_kind != TYPE_INFERRED_VECTOR) return false;
				compare_len = false;
				break;
			default:
				return false;
		}
	}
	if (compare_len && to->array.len != from->array.len) return false;

	Type *from_base = from->array.base;
	Type *to_base = to->array.base;
	if (is_explicit)
	{
		from_base = type_flatten(from_base);
		to_base = type_flatten(to_base);
	}

	if (from_base == to_base) return true;

	switch (to_base->type_kind)
	{
		case TYPE_POINTER:
			if (from_base->type_kind == TYPE_POINTER)
			{
				if (is_explicit) return true;
				return cast_may_implicit_ptr(to_base->pointer, from_base->pointer);
			}
			return false;
		case TYPE_ARRAY:
		case TYPE_INFERRED_ARRAY:
		case TYPE_VECTOR:
		case TYPE_INFERRED_VECTOR:
			to = to_base;
			from = from_base;
			goto RETRY;
		default:
			return is_explicit && type_is_structurally_equivalent(to_base, from_base);
	}
}

static inline bool cast_may_implicit_ptr(Type *from_pointee, Type *to_pointee)
{
	assert(!type_is_optional(from_pointee) && !type_is_optional(to_pointee) && "Optional should already been handled");
	if (from_pointee == to_pointee) return true;

	// For void* on either side, no checks.
	if (to_pointee == type_voidptr || from_pointee == type_voidptr) return true;

	// Step through all *:
	while (from_pointee->type_kind == TYPE_POINTER && to_pointee->type_kind == TYPE_POINTER)
	{
		if (from_pointee == type_voidptr || to_pointee == type_voidptr) return true;
		from_pointee = from_pointee->pointer;
		to_pointee = to_pointee->pointer;
	}

	assert(to_pointee != from_pointee);

	// Functions compare raw types.
	if (from_pointee->type_kind == TYPE_FUNC && to_pointee->type_kind == TYPE_FUNC)
	{
		return to_pointee->function.prototype->raw_type == from_pointee->function.prototype->raw_type;
	}

	// Special handling of int* = int[4]* (so we have int[4] -> int)
	if (type_is_arraylike(from_pointee))
	{
		if (cast_may_implicit_ptr(to_pointee, from_pointee->array.base)) return true;
	}

	if (type_is_any_arraylike(to_pointee) || type_is_any_arraylike(from_pointee))
	{
		return cast_may_array(from_pointee, to_pointee, false);
	}
	// Use subtype matching
	return type_is_subtype(to_pointee, from_pointee);
}

/**
 * Can the conversion occur implicitly?
 */
bool cast_may_implicit(Type *from_type, Type *to_type, CastOption option)
{
	Type *to = to_type->canonical;

	// First handle void! => any error
	if (to == type_anyerr && type_may_convert_to_anyerr(from_type)) return true;

	// any! => may implicitly to convert to any.
	if (type_is_optional_any(from_type)) return (CAST_OPTION_ALLOW_OPTIONAL & option) != 0;

	// Check if optional was allowed
	Type *from = from_type->canonical;
	if (type_is_optional_type(from_type))
	{
		if (!(CAST_OPTION_ALLOW_OPTIONAL & option)) return false;
		from = from_type->failable->canonical;
	}

	// Same canonical type - we're fine.
	if (from == to) return true;

	// Handle floats
	if (type_is_float(to))
	{
		// 2a. Any integer may convert to a float.
		if (type_is_integer(from)) return true;

		// 2b. Any narrower float
		if (type_is_float(from))
		{
			ByteSize to_size = type_size(to);
			ByteSize from_size = type_size(from);
			if (to_size == from_size) return true;
			return to_size > from_size && (option & CAST_OPTION_SIMPLE_EXPR);
		}
		return false;
	}

	// Handle ints
	if (type_is_integer(to))
	{
		// For an enum, lower to the underlying enum type.
		if (from->type_kind == TYPE_ENUM)
		{
			from = from->decl->enums.type_info->type->canonical;
		}

		// 3a. Any narrower int may convert to a wider or same int, regardless of signedness.
		if (type_is_integer(from))
		{
			ByteSize to_size = type_size(to);
			ByteSize from_size = type_size(from);
			if (to_size == from_size) return true;
			return to_size > from_size && (option & CAST_OPTION_SIMPLE_EXPR);
		}
		return false;
	}

	// Convert any fault to anyerr
	if (to == type_anyerr && from->type_kind == TYPE_FAULTTYPE) return true;

	// Handle pointers
	if (type_is_pointer(to))
	{
		// Assigning a subarray to a pointer
		if (from->type_kind == TYPE_SUBARRAY)
		{
			// Casting to a void* always works.
			if (to == type_voidptr) return true;

			// Compare as if it was a pointer.
			return cast_may_implicit_ptr(from->array.base, to_type->pointer);
		}

		// Assigning a pointer to a pointer
		if (from->type_kind == TYPE_POINTER)
		{
			// Casting to or from a void* always works.
			if (to == type_voidptr || from == type_voidptr) return true;

			return cast_may_implicit_ptr(from->pointer, to->pointer);
		}
		return false;
	}

	if (type_is_any_arraylike(to) && type_is_any_arraylike(from))
	{
		return cast_may_array(from, to, false);
	}

	// 5. Handle sub arrays
	if (to->type_kind == TYPE_SUBARRAY)
	{
		// 5a. char[] foo = "test"
		Type *base = to->array.base;

		// 5b. Assign sized array pointer int[] = int[4]*
		if (type_is_pointer(from))
		{
			return from->pointer->type_kind == TYPE_ARRAY && from->pointer->array.base == base;
		}

		// Allow casting void*[] to int*[]
		if (from->type_kind == TYPE_SUBARRAY && from->array.base == type_voidptr && type_is_pointer(to->array.base))
		{
			return true;
		}
		// Allow casting int*[] -> void*[]
		if (from->type_kind == TYPE_SUBARRAY && to->array.base == type_voidptr && type_is_pointer(from->array.base))
		{
			return true;
		}
		return false;
	}



	// 8. Check if we may cast this to bool. It is safe for many types.
	if (to->type_kind == TYPE_BOOL)
	{
		return cast_to_bool_kind(from) != CAST_ERROR;
	}

	// 9. Any cast
	if (to->type_kind == TYPE_ANY)
	{
		return from->type_kind == TYPE_POINTER;
	}


	// 11. Substruct cast, if the first member is inline, see if we can cast to this member.
	if (type_is_substruct(from))
	{
		return cast_may_implicit(from->decl->strukt.members[0]->type, to, option);
	}

	return false;
}

bool may_convert_float_const_implicit(Expr *expr, Type *to_type)
{
	if (!expr_const_float_fits_type(&expr->const_expr, type_flatten(to_type)->type_kind))
	{
		SEMA_ERROR(expr, "The value '%g' is out of range for %s, so you need an explicit cast to truncate the value.", expr->const_expr.fxx.f, type_quoted_error_string(to_type));
		return false;
	}
	return true;
}


bool may_convert_int_const_implicit(Expr *expr, Type *to_type)
{
	Type *to_type_flat = type_flatten(to_type);
	switch (to_type_flat->type_kind)
	{
		case ALL_FLOATS:
		case TYPE_BOOL:
			return true;
		case ALL_INTS:
			break;
		default:
			return false;
	}
	if (expr_const_will_overflow(&expr->const_expr, to_type_flat->type_kind))
	{
		sema_error_const_int_out_of_range(expr, expr, to_type);
		return false;
	}
	return true;
}

INLINE Expr *recursive_may_narrow_floatid(ExprId expr, Type *type)
{
	return recursive_may_narrow_float(exprptr(expr), type);
}

Expr *recursive_may_narrow_float(Expr *expr, Type *type)
{
	switch (expr->expr_kind)
	{
		case EXPR_BINARY:
		case EXPR_BITASSIGN:
			switch (expr->binary_expr.operator)
			{
				case BINARYOP_ERROR:
					UNREACHABLE
				case BINARYOP_MULT:
				case BINARYOP_SUB:
				case BINARYOP_ADD:
				case BINARYOP_DIV:
				case BINARYOP_MOD:
				case BINARYOP_ELSE:
				{
					Expr *res = recursive_may_narrow_float(exprptr(expr->binary_expr.left), type);
					if (res) return res;
					return recursive_may_narrow_float(exprptr(expr->binary_expr.right), type);
				}
				case BINARYOP_BIT_OR:
				case BINARYOP_BIT_XOR:
				case BINARYOP_BIT_AND:
				case BINARYOP_AND:
				case BINARYOP_OR:
				case BINARYOP_GT:
				case BINARYOP_GE:
				case BINARYOP_LT:
				case BINARYOP_LE:
				case BINARYOP_NE:
				case BINARYOP_EQ:
				case BINARYOP_SHR:
				case BINARYOP_SHL:
				case BINARYOP_BIT_AND_ASSIGN:
				case BINARYOP_BIT_OR_ASSIGN:
				case BINARYOP_BIT_XOR_ASSIGN:
				case BINARYOP_SHR_ASSIGN:
				case BINARYOP_SHL_ASSIGN:
					UNREACHABLE
				case BINARYOP_ASSIGN:
				case BINARYOP_ADD_ASSIGN:
				case BINARYOP_DIV_ASSIGN:
				case BINARYOP_MOD_ASSIGN:
				case BINARYOP_MULT_ASSIGN:
				case BINARYOP_SUB_ASSIGN:
					return recursive_may_narrow_float(exprptr(expr->binary_expr.left), type);
			}
			UNREACHABLE
		case EXPR_MACRO_BODY_EXPANSION:
		case EXPR_CALL:
		case EXPR_POISONED:
		case EXPR_BITACCESS:
		case EXPR_ACCESS:
		case EXPR_CATCH_UNWRAP:
		case EXPR_COMPOUND_LITERAL:
		case EXPR_COND:
		case EXPR_DECL:
		case EXPR_CT_IDENT:
		case EXPR_DESIGNATOR:
		case EXPR_EXPR_BLOCK:
		case EXPR_MACRO_BLOCK:
		case EXPR_IDENTIFIER:
		case EXPR_SLICE_ASSIGN:
		case EXPR_SLICE_COPY:
		case EXPR_SLICE:
		case EXPR_SUBSCRIPT:
		case EXPR_RETVAL:
		case EXPR_TYPEID_INFO:
			if (type_size(expr->type) > type_size(type)) return expr;
			return NULL;
		case EXPR_EXPRESSION_LIST:
			return recursive_may_narrow_float(VECLAST(expr->expression_list), type);
		case EXPR_GROUP:
		case EXPR_FORCE_UNWRAP:
			return recursive_may_narrow_float(expr->inner_expr, type);
		case EXPR_RETHROW:
			return recursive_may_narrow_float(expr->rethrow_expr.inner, type);
		case EXPR_TERNARY:
		{
			Expr *res = recursive_may_narrow_floatid(expr->ternary_expr.then_expr ? expr->ternary_expr.then_expr
			                                                                      : expr->ternary_expr.cond, type);
			if (res) return res;
			return recursive_may_narrow_floatid(expr->ternary_expr.else_expr, type);
		}
		case EXPR_CAST:
			if (expr->cast_expr.implicit)
			{
				return recursive_may_narrow_floatid(expr->cast_expr.expr, type);
			}
			return type_size(expr->type) > type_size(type) ? expr : NULL;
		case EXPR_CONST:
			if (!expr->const_expr.narrowable)
			{
				return type_size(expr->type) > type_size(type) ? expr : NULL;
			}
			assert(expr->const_expr.const_kind == CONST_FLOAT);
			if (!expr_const_float_fits_type(&expr->const_expr, type_flatten(type)->type_kind))
			{
				return expr;
			}
			return NULL;
		case EXPR_FAILABLE:
		case EXPR_HASH_IDENT:
		case EXPR_FLATPATH:
		case EXPR_INITIALIZER_LIST:
		case EXPR_DESIGNATED_INITIALIZER_LIST:
		case EXPR_TYPEID:
		case EXPR_TYPEINFO:
		case EXPR_CT_CALL:
		case EXPR_NOP:
		case EXPR_CATCH:
		case EXPR_BUILTIN:
		case EXPR_TRY_UNWRAP:
		case EXPR_TRY_UNWRAP_CHAIN:
		case EXPR_SUBSCRIPT_ADDR:
		case EXPR_VARIANTSWITCH:
		case EXPR_ARGV_TO_SUBARRAY:
		case EXPR_COMPILER_CONST:
		case EXPR_STRINGIFY:
		case EXPR_CT_EVAL:
		case EXPR_VARIANT:
		case EXPR_POINTER_OFFSET:
		case EXPR_CT_ARG:
		case EXPR_ASM:
		case EXPR_VASPLAT:
		case EXPR_OPERATOR_CHARS:
		case EXPR_CT_CHECKS:
		case EXPR_SUBSCRIPT_ASSIGN:
			UNREACHABLE
		case EXPR_BUILTIN_ACCESS:
		case EXPR_TEST_HOOK:
			return false;
		case EXPR_POST_UNARY:
			return recursive_may_narrow_float(expr->unary_expr.expr, type);
		case EXPR_TRY:
			return recursive_may_narrow_float(expr->inner_expr, type);
		case EXPR_UNARY:
		{
			switch (expr->unary_expr.operator)
			{
				case UNARYOP_DEREF:
					return false;
				case UNARYOP_ERROR:
				case UNARYOP_ADDR:
				case UNARYOP_NOT:
				case UNARYOP_TADDR:
					UNREACHABLE
				case UNARYOP_NEG:
				case UNARYOP_BITNEG:
				case UNARYOP_INC:
				case UNARYOP_DEC:
					return recursive_may_narrow_float(expr->unary_expr.expr, type);
			}
		}
	}
	UNREACHABLE
}

INLINE Expr *recursive_may_narrow_intid(ExprId expr, Type *type)
{
	assert(expr);
	return recursive_may_narrow_int(exprptr(expr), type);
}

Expr *recursive_may_narrow_int(Expr *expr, Type *type)
{
	switch (expr->expr_kind)
	{
		case EXPR_BITASSIGN:
		case EXPR_BINARY:
			switch (expr->binary_expr.operator)
			{
				case BINARYOP_ERROR:
					UNREACHABLE
				case BINARYOP_MULT:
				case BINARYOP_SUB:
				case BINARYOP_ADD:
				case BINARYOP_DIV:
				case BINARYOP_MOD:
				case BINARYOP_BIT_OR:
				case BINARYOP_BIT_XOR:
				case BINARYOP_BIT_AND:
				case BINARYOP_ELSE:
				{
					Expr *res = recursive_may_narrow_int(exprptr(expr->binary_expr.left), type);
					if (res) return res;
					return recursive_may_narrow_int(exprptr(expr->binary_expr.right), type);
				}
				case BINARYOP_AND:
				case BINARYOP_OR:
				case BINARYOP_GT:
				case BINARYOP_GE:
				case BINARYOP_LT:
				case BINARYOP_LE:
				case BINARYOP_NE:
				case BINARYOP_EQ:
					return NULL;
				case BINARYOP_SHR:
				case BINARYOP_SHL:
				case BINARYOP_ASSIGN:
				case BINARYOP_ADD_ASSIGN:
				case BINARYOP_BIT_AND_ASSIGN:
				case BINARYOP_BIT_OR_ASSIGN:
				case BINARYOP_BIT_XOR_ASSIGN:
				case BINARYOP_DIV_ASSIGN:
				case BINARYOP_MOD_ASSIGN:
				case BINARYOP_MULT_ASSIGN:
				case BINARYOP_SHR_ASSIGN:
				case BINARYOP_SHL_ASSIGN:
				case BINARYOP_SUB_ASSIGN:
					return recursive_may_narrow_int(exprptr(expr->binary_expr.left), type);
			}
			UNREACHABLE
		case EXPR_MACRO_BODY_EXPANSION:
		case EXPR_CALL:
		case EXPR_POISONED:
		case EXPR_BITACCESS:
		case EXPR_ACCESS:
		case EXPR_CATCH_UNWRAP:
		case EXPR_COMPOUND_LITERAL:
		case EXPR_COND:
		case EXPR_DECL:
		case EXPR_CT_IDENT:
		case EXPR_DESIGNATOR:
		case EXPR_EXPR_BLOCK:
		case EXPR_MACRO_BLOCK:
		case EXPR_IDENTIFIER:
		case EXPR_SLICE_ASSIGN:
		case EXPR_SLICE_COPY:
		case EXPR_SLICE:
		case EXPR_SUBSCRIPT:
		case EXPR_RETVAL:
		case EXPR_SUBSCRIPT_ASSIGN:
		case EXPR_TYPEID_INFO:
			if (type_size(expr->type) > type_size(type)) return expr;
			return NULL;
		case EXPR_BUILTIN_ACCESS:
			switch (expr->builtin_access_expr.kind)
			{
				case ACCESS_LEN:
					if (type_size(type) < type_size(type_cint)) return expr;
					return NULL;
				case ACCESS_TYPEOFANY:
				case ACCESS_PTR:
				case ACCESS_ENUMNAME:
				case ACCESS_FAULTNAME:
					return NULL;
			}
			UNREACHABLE;
		case EXPR_EXPRESSION_LIST:
			return recursive_may_narrow_int(VECLAST(expr->expression_list), type);
		case EXPR_RETHROW:
			return recursive_may_narrow_int(expr->rethrow_expr.inner, type);
		case EXPR_TERNARY:
		{
			Expr *res = recursive_may_narrow_intid(expr->ternary_expr.then_expr ? expr->ternary_expr.then_expr
			                                                                  : expr->ternary_expr.cond, type);
			if (res) return res;
			return recursive_may_narrow_intid(expr->ternary_expr.else_expr, type);
		}
		case EXPR_CAST:
			if (expr->cast_expr.implicit)
			{
				return recursive_may_narrow_intid(expr->cast_expr.expr, type);
			}
			return type_size(expr->type) > type_size(type) ? expr : NULL;
		case EXPR_CONST:
			if (!expr->const_expr.narrowable)
			{
				return type_size(expr->type) > type_size(type) ? expr : NULL;
			}
			assert(expr->const_expr.const_kind == CONST_INTEGER);
			if (expr_const_will_overflow(&expr->const_expr, type_flatten(type)->type_kind))
			{
				return expr;
			}
			return NULL;
		case EXPR_FAILABLE:
		case EXPR_HASH_IDENT:
		case EXPR_FLATPATH:
		case EXPR_INITIALIZER_LIST:
		case EXPR_DESIGNATED_INITIALIZER_LIST:
		case EXPR_TYPEID:
		case EXPR_TYPEINFO:
		case EXPR_CT_CALL:
		case EXPR_NOP:
		case EXPR_BUILTIN:
		case EXPR_TRY_UNWRAP:
		case EXPR_TRY_UNWRAP_CHAIN:
		case EXPR_SUBSCRIPT_ADDR:
		case EXPR_ARGV_TO_SUBARRAY:
		case EXPR_VARIANTSWITCH:
		case EXPR_COMPILER_CONST:
		case EXPR_STRINGIFY:
		case EXPR_CT_EVAL:
		case EXPR_VARIANT:
		case EXPR_POINTER_OFFSET:
		case EXPR_CT_ARG:
		case EXPR_ASM:
		case EXPR_VASPLAT:
		case EXPR_OPERATOR_CHARS:
		case EXPR_CT_CHECKS:
			UNREACHABLE
		case EXPR_TEST_HOOK:
			return false;
		case EXPR_POST_UNARY:
			return recursive_may_narrow_int(expr->unary_expr.expr, type);
		case EXPR_TRY:
		case EXPR_CATCH:
		case EXPR_GROUP:
		case EXPR_FORCE_UNWRAP:
			return recursive_may_narrow_int(expr->inner_expr, type);
		case EXPR_UNARY:
		{
			switch (expr->unary_expr.operator)
			{
				case UNARYOP_ERROR:
				case UNARYOP_DEREF:
				case UNARYOP_ADDR:
				case UNARYOP_NOT:
				case UNARYOP_TADDR:
					UNREACHABLE
				case UNARYOP_NEG:
				case UNARYOP_BITNEG:
				case UNARYOP_INC:
				case UNARYOP_DEC:
					return recursive_may_narrow_int(expr->unary_expr.expr, type);
			}
		}
	}
	UNREACHABLE
}

static void sema_error_const_int_out_of_range(Expr *expr, Expr *problem, Type *to_type)
{
	assert(expr->expr_kind == EXPR_CONST);
	if (expr->const_expr.is_character)
	{
		SEMA_ERROR(problem, "The unicode character U+%04x cannot fit in a %s.", (uint32_t)expr->const_expr.ixx.i.low, type_quoted_error_string(to_type));
		return;
	}
	const char *error_value = expr->const_expr.is_hex ? int_to_str(expr->const_expr.ixx, 16) : expr_const_to_error_string(&expr->const_expr);
	SEMA_ERROR(problem, "The value '%s' is out of range for %s, so you need an explicit cast to truncate the value.", error_value,
	           type_quoted_error_string(to_type));
}

static inline bool cast_maybe_null_to_distinct_voidptr(Expr *expr, Type *expr_canonical, Type *to_canonical)
{
	if (expr->expr_kind != EXPR_CONST || expr->const_expr.const_kind != CONST_POINTER) return false;
	if (expr_canonical != type_voidptr) return false;
	if (expr->const_expr.ptr) return false;
	if (to_canonical->type_kind != TYPE_DISTINCT) return false;
	return to_canonical->decl->distinct_decl.base_type->canonical->type_kind == TYPE_POINTER;
}

static inline bool cast_maybe_string_lit_to_char_array(Expr *expr, Type *expr_canonical, Type *to_canonical)
{
	if (expr->expr_kind != EXPR_CONST || expr->const_expr.const_kind != CONST_STRING) return false;
	if (expr_canonical->type_kind != TYPE_POINTER) return false;
	if (to_canonical->type_kind != TYPE_ARRAY && to_canonical->type_kind != TYPE_INFERRED_ARRAY) return false;
	if (to_canonical->array.base != type_char) return false;
	Type *pointer = expr_canonical->pointer;
	if (pointer->type_kind != TYPE_ARRAY) return false;
	if (pointer->array.base != type_char) return false;
	if (to_canonical->type_kind == TYPE_INFERRED_ARRAY)
	{
		to_canonical = type_get_array(to_canonical->array.base, pointer->array.len);
	}
	expr->type = to_canonical;
	return true;
}

bool cast_untyped_to_type(SemaContext *context, Expr *expr, Type *to_type)
{
	recursively_rewrite_untyped_list(expr, expr->const_expr.untyped_list);
	if (!sema_expr_analyse_initializer_list(context, type_flatten(to_type), expr)) return false;
	expr->type = to_type;
	return true;
}

static void recursively_rewrite_untyped_list(Expr *expr, Expr **list)
{
	expr->expr_kind = EXPR_INITIALIZER_LIST;
	expr->initializer_list = list;
	expr->resolve_status = RESOLVE_NOT_DONE;
	FOREACH_BEGIN(Expr *inner, list)
		if (expr_is_const_untyped_list(inner))
		{
			recursively_rewrite_untyped_list(inner, inner->const_expr.untyped_list);
		}
	FOREACH_END();
}

bool cast_implicit(SemaContext *context, Expr *expr, Type *to_type)
{
	assert(!type_is_optional(to_type));
	Type *expr_type = expr->type;
	Type *expr_canonical = expr_type->canonical;
	Type *to_canonical = to_type->canonical;
	if (cast_maybe_string_lit_to_char_array(expr, expr_canonical, to_canonical))
	{
		expr_type = expr->type;
		expr_canonical = expr_type->canonical;
	}
	if (cast_maybe_null_to_distinct_voidptr(expr, expr_canonical, to_canonical))
	{
		return true;
	}
	if (expr_canonical == to_canonical) return true;
	if (expr_type == type_untypedlist)
	{
		return cast_untyped_to_type(context, expr, to_type);
	}

	CastOption option = CAST_OPTION_ALLOW_OPTIONAL;
	if (expr_is_simple(expr)) option |= CAST_OPTION_SIMPLE_EXPR;

	if (!cast_may_implicit(expr_canonical, to_canonical, option))
	{
		if (!cast_may_explicit(expr_canonical, to_canonical, false, expr->expr_kind == EXPR_CONST))
		{
			if (expr_canonical->type_kind == TYPE_OPTIONAL && to_canonical->type_kind != TYPE_OPTIONAL)
			{
				SEMA_ERROR(expr, "An optional %s cannot be converted to %s.", type_quoted_error_string(expr->type), type_quoted_error_string(to_type));
				return false;
			}
			if (to_canonical->type_kind == TYPE_ANY)
			{
				SEMA_ERROR(expr, "You can only convert pointers to 'variant', take the address of this expression first.");
				return false;
			}
			SEMA_ERROR(expr, "You cannot cast %s into %s even with an explicit cast, so this looks like an error.", type_quoted_error_string(expr->type), type_quoted_error_string(to_type));
			return false;
		}
		bool is_narrowing = type_size(expr_canonical) >= type_size(to_canonical);
		if (expr->expr_kind == EXPR_CONST && expr->const_expr.narrowable && is_narrowing)
		{
			Type *expr_flatten = type_flatten_distinct(expr_canonical);
			Type *to_flatten = type_flatten_distinct(to_canonical);
			if (type_is_integer(expr_flatten) && type_is_integer(to_flatten))
			{
				Expr *problem = recursive_may_narrow_int(expr, to_canonical);
				if (problem)
				{
					sema_error_const_int_out_of_range(expr, problem, to_type);
					return false;
				}
				goto OK;
			}
			if (type_is_float(expr_flatten) && type_is_float(to_flatten))
			{
				Expr *problem = recursive_may_narrow_float(expr, to_canonical);
				if (problem)
				{
					SEMA_ERROR(problem, "The value '%s' is out of range for %s, so you need an explicit cast to truncate the value.", expr_const_to_error_string(&expr->const_expr),
					           type_quoted_error_string(to_type));
					return false;
				}
				goto OK;
			}
		}
		if (type_is_integer(expr_canonical) && type_is_integer(to_canonical) && is_narrowing)
		{
			Expr *problem = recursive_may_narrow_int(expr, to_canonical);
			if (problem)
			{
				SEMA_ERROR(problem, "Cannot narrow %s to %s.", type_quoted_error_string(problem->type),
				           type_quoted_error_string(to_type));
				return false;
			}
			goto OK;
		}
		if (type_is_float(expr_canonical) && type_is_float(to_canonical) && is_narrowing)
		{
			Expr *problem = recursive_may_narrow_float(expr, to_canonical);
			if (problem)
			{
				if (problem->expr_kind == EXPR_CONST)
				{
					SEMA_ERROR(problem, "The value '%s' is out of range for %s.", expr_const_to_error_string(&problem->const_expr),
					           type_quoted_error_string(to_type));
				}
				else
				{
					SEMA_ERROR(problem, "This expression cannot be implicitly narrowed.");
				}
				return false;
			}
			goto OK;
		}
		SEMA_ERROR(expr, "Implicitly casting %s to %s is not permitted, but you can do an explicit cast by placing '(%s)' before the expression.", type_quoted_error_string(
				type_no_optional(expr->type)), type_quoted_error_string(type_no_optional(to_type)),
		           type_to_error_string(to_type));
		return false;
	}

	OK:
	// Additional checks for compile time values.
	if (expr->expr_kind == EXPR_CONST && expr->const_expr.narrowable)
	{
		if (type_is_float(expr->type))
		{
			if (!may_convert_float_const_implicit(expr, to_type)) return false;
		}
		else if (type_is_integer(expr->type))
		{
			if (!may_convert_int_const_implicit(expr, to_type)) return false;
		}
	}
	cast(expr, to_type);
	// Allow narrowing after widening
	if (type_is_numeric(to_type) && expr->expr_kind == EXPR_CONST && type_size(expr_canonical) < type_size(to_canonical))
	{
		expr->const_expr.narrowable = true;
	}
	if (expr->expr_kind == EXPR_CAST) expr->cast_expr.implicit = true;
	return true;
}
static bool arr_to_vec(Expr *expr, Type *to_type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_ARRVEC, to_type)) return true;

	assert(expr->const_expr.const_kind == CONST_INITIALIZER);
	ConstInitializer *list = expr->const_expr.initializer;
	list->type = to_type;
	expr->type = to_type;
	return true;
}

static bool vec_to_arr(Expr *expr, Type *to_type)
{
	if (insert_runtime_cast_unless_const(expr, CAST_VECARR, to_type)) return true;

	assert(expr->const_expr.const_kind == CONST_INITIALIZER);
	ConstInitializer *list = expr->const_expr.initializer;
	list->type = to_type;
	expr->type = to_type;
	return true;
}

static bool err_to_anyerr(Expr *expr, Type *to_type)
{
	expr->type = to_type;
	return true;
}

static bool err_to_bool(Expr *expr, Type *to_type)
{
	if (expr->expr_kind == EXPR_CONST)
	{
		switch (expr->const_expr.const_kind)
		{
			case CONST_INTEGER:
				return int_literal_to_bool(expr, to_type);
			case CONST_ERR:
				expr_rewrite_const_bool(expr, type_bool, expr->const_expr.enum_err_val != NULL);
				return true;
			default:
				UNREACHABLE
		}
	}
	return insert_cast(expr, CAST_ERBOOL, to_type);
}

static inline bool subarray_to_subarray(Expr *expr, Type *to_type)
{
	if (expr_is_const(expr))
	{
		expr->type = to_type;
	}
	return insert_cast(expr, CAST_SASA, to_type);
}
static inline bool subarray_to_bool(Expr *expr)
{
	if (expr->expr_kind == EXPR_CONST && expr->const_expr.const_kind == CONST_INITIALIZER)
	{
		ConstInitializer *list = expr->const_expr.initializer;
		switch (list->kind)
		{
			case CONST_INIT_ZERO:
				expr_rewrite_const_bool(expr, type_bool, false);
				return true;
			case CONST_INIT_ARRAY:
				expr_rewrite_const_bool(expr, type_bool, vec_size(list->init_array.elements) > 0);
				return true;
			case CONST_INIT_ARRAY_FULL:
				expr_rewrite_const_bool(expr, type_bool, vec_size(list->init_array_full) > 0);
				return true;
			case CONST_INIT_STRUCT:
			case CONST_INIT_UNION:
			case CONST_INIT_VALUE:
			case CONST_INIT_ARRAY_VALUE:
				break;
		}
	}
	return insert_cast(expr, CAST_SABOOL, type_bool);
}

static bool cast_inner(Expr *expr, Type *from_type, Type *to, Type *to_type)
{
	switch (from_type->type_kind)
	{
		case TYPE_FAILABLE_ANY:
		case TYPE_OPTIONAL:
			UNREACHABLE
		case TYPE_VOID:
			UNREACHABLE
		case TYPE_DISTINCT:
		case TYPE_FUNC:
		case TYPE_TYPEDEF:
		case CT_TYPES:
			UNREACHABLE
		case TYPE_BITSTRUCT:
			return bitstruct_cast(expr, from_type, to, to_type);
		case TYPE_BOOL:
			// Bool may convert into integers and floats but only explicitly.
			if (type_is_integer(to)) return bool_to_int(expr, to, to_type);
			if (type_is_float(to)) return bool_to_float(expr, to, to_type);
			break;
		case TYPE_ANYERR:
			if (to->type_kind == TYPE_BOOL) return insert_cast(expr, CAST_EUBOOL, to_type);
			if (to->type_kind == TYPE_FAULTTYPE) return insert_cast(expr, CAST_EUER, to_type);
			if (type_is_integer(to)) return insert_cast(expr, CAST_EUINT, to_type);
			break;
		case ALL_SIGNED_INTS:
			if (type_is_integer_unsigned(to)) return int_conversion(expr, CAST_SIUI, to, to_type);
			if (type_is_integer_signed(to)) return int_conversion(expr, CAST_SISI, to, to_type);
			if (type_is_float(to)) return int_to_float(expr, CAST_SIFP, to, to_type);
			if (to == type_bool) return integer_to_bool(expr, to_type);
			if (to->type_kind == TYPE_POINTER) return int_to_pointer(expr, to_type);
			if (to->type_kind == TYPE_ENUM) return integer_to_enum(expr, to, to_type);
			break;
		case ALL_UNSIGNED_INTS:
			if (type_is_integer_unsigned(to)) return int_conversion(expr, CAST_UIUI, to, to_type);
			if (type_is_integer_signed(to)) return int_conversion(expr, CAST_UISI, to, to_type);
			if (type_is_float(to)) return int_to_float(expr, CAST_UIFP, to, to_type);
			if (to == type_bool) return integer_to_bool(expr, to_type);
			if (to->type_kind == TYPE_POINTER) return int_to_pointer(expr, to_type);
			if (to->type_kind == TYPE_ENUM) return integer_to_enum(expr, to, to_type);
			break;
		case ALL_FLOATS:
			if (type_is_integer(to)) return float_to_integer(expr, to, to_type);
			if (to == type_bool) return float_to_bool(expr, to_type);
			if (type_is_float(to)) return float_to_float(expr, to, to_type);
			break;
		case TYPE_TYPEID:
		case TYPE_POINTER:
			if (type_is_integer(to)) return pointer_to_integer(expr, to_type);
			if (to->type_kind == TYPE_BOOL) return pointer_to_bool(expr, to_type);
			if (to->type_kind == TYPE_POINTER) return pointer_to_pointer(expr, to_type);
			if (to->type_kind == TYPE_SUBARRAY) return insert_cast(expr, CAST_APTSA, to_type);
			if (to->type_kind == TYPE_ANY) return insert_cast(expr, CAST_PTRANY, to_type);
			break;
		case TYPE_ANY:
			if (to->type_kind == TYPE_POINTER) return insert_cast(expr, CAST_ANYPTR, to_type);
			break;
		case TYPE_ENUM:
			if (type_is_integer(to)) return enum_to_integer(expr, from_type, to, to_type);
			if (type_is_float(to)) return enum_to_float(expr, from_type, to, to_type);
			if (to == type_bool) return enum_to_bool(expr, from_type, to_type);
			if (to->type_kind == TYPE_POINTER) return enum_to_pointer(expr, from_type, to_type);
			break;
		case TYPE_FAULTTYPE:
			if (to->type_kind == TYPE_ANYERR) return err_to_anyerr(expr, to_type);
			if (to == type_bool) return err_to_bool(expr, to_type);
			if (type_is_integer(to)) return insert_cast(expr, CAST_ERINT, to_type);
			break;
		case TYPE_FLEXIBLE_ARRAY:
		case TYPE_SCALED_VECTOR:
			return false;
		case TYPE_ARRAY:
			if (to->type_kind == TYPE_VECTOR) return arr_to_vec(expr, to_type);
			FALLTHROUGH;
		case TYPE_STRUCT:
		case TYPE_UNION:

			if (to->type_kind == TYPE_ARRAY || to->type_kind == TYPE_STRUCT || to->type_kind == TYPE_UNION)
			{
				return insert_cast(expr, CAST_STST, to_type);
			} // Starting in a little while...
			break;
		case TYPE_SUBARRAY:
			if (to->type_kind == TYPE_POINTER) return insert_cast(expr, CAST_SAPTR, to);
			if (to->type_kind == TYPE_BOOL) return subarray_to_bool(expr);
			if (to->type_kind == TYPE_SUBARRAY) return subarray_to_subarray(expr, to);
			break;
		case TYPE_VECTOR:
			if (to->type_kind == TYPE_ARRAY) return vec_to_arr(expr, to_type);
			break;
	}
	UNREACHABLE
}

static bool bitstruct_cast(Expr *expr, Type *from_type, Type *to, Type *to_type)
{
	Type *base_type = type_flatten_distinct(from_type->decl->bitstruct.base_type->type);
	assert(type_size(to) == type_size(base_type));
	if (type_is_integer(base_type) && type_is_integer(to))
	{
		expr->type = to_type;
		return true;
	}
	if (base_type->type_kind == TYPE_ARRAY && to->type_kind == TYPE_ARRAY)
	{
		expr->type = to_type;
		return true;
	}
	if (type_is_integer(base_type))
	{
		assert(to->type_kind == TYPE_ARRAY);
		return insert_cast(expr, CAST_BSARRY, to_type);
	}
	assert(base_type->type_kind == TYPE_ARRAY);
	return insert_cast(expr, CAST_BSINT, to_type);
}

bool cast(Expr *expr, Type *to_type)
{
	assert(!type_is_optional(to_type));
	Type *from_type = expr->type;
	bool from_is_failable = false;
	Type *to = type_flatten_distinct(to_type);

	// Special case *! => error
	if (to == type_anyerr || to->type_kind == TYPE_FAULTTYPE)
	{
		if (type_is_optional(from_type)) return voidfail_to_error(expr, to_type);
	}

	if (type_is_optional_any(from_type))
	{
		expr->type = type_get_optional(to_type);
		return true;
	}

	if (type_is_optional_type(from_type))
	{
		from_type = from_type->failable;
		from_is_failable = true;
	}
	from_type = type_flatten_distinct(from_type);
	if (type_len_is_inferred(to_type))
	{
		to_type = from_type;
		to = type_flatten(from_type);
	}
	if (from_type == to)
	{
		expr->type = type_add_optional(to_type, from_is_failable);
		if (expr->expr_kind == EXPR_CONST)
		{
			expr->const_expr.narrowable = false;
			expr->const_expr.is_hex = false;
		}
		return true;
	}

	if (!cast_inner(expr, from_type, to, to_type)) return false;

	Type *result_type = expr->type;
	if (from_is_failable && !type_is_optional(result_type))
	{
		expr->type = type_get_optional(result_type);
	}
	return true;
}

bool cast_to_index(Expr *index)
{
	Type *type = index->type->canonical;
	RETRY:
	switch (type->type_kind)
	{
		case TYPE_I8:
		case TYPE_I16:
		case TYPE_I32:
		case TYPE_I64:
			return cast(index, type_isz);
		case TYPE_U8:
		case TYPE_U16:
		case TYPE_U32:
		case TYPE_U64:
			return cast(index, type_usz);
		case TYPE_U128:
			SEMA_ERROR(index, "You need to explicitly cast this to a uint or ulong.");
			return false;
		case TYPE_I128:
			SEMA_ERROR(index, "index->type->canonical this to an int or long.");
			return false;
		case TYPE_ENUM:
			type = type->decl->enums.type_info->type->canonical;
			goto RETRY;
		default:
			SEMA_ERROR(index, "Cannot implicitly convert '%s' to an index.", type_to_error_string(index->type));
			return false;
	}
}

bool cast_widen_top_down(SemaContext *context, Expr *expr, Type *type)
{
	Type *to = type;
	Type *from = expr->type;
	RETRY:
	if (type_is_integer(from) && type_is_integer(to)) goto CONVERT_IF_BIGGER;
	if (type_is_float(from) && type_is_float(to)) goto CONVERT_IF_BIGGER;
	if (type_is_integer(from) && type_is_float(to)) goto CONVERT;
	if (type_flat_is_vector(from) && type_flat_is_vector(to))
	{
		to = type_vector_type(to);
		from = type_vector_type(from);
		goto RETRY;
	}
	return true;
	CONVERT_IF_BIGGER:
	if (type_size(to) <= type_size(from)) return true;
	CONVERT:
	return cast_implicit(context, expr, type);
}

bool cast_promote_vararg(Expr *arg)
{
	Type *arg_type = arg->type->canonical;

	// 2. Promote any integer or bool to at least CInt
	if (type_is_promotable_integer(arg_type) || arg_type == type_bool)
	{
		return cast(arg, type_cint);
	}
	// 3. Promote any float to at least double
	if (type_is_promotable_float(arg->type))
	{
		return cast(arg, type_double);
	}
	return true;
}

Type *cast_numeric_arithmetic_promotion(Type *type)
{
	if (!type) return NULL;
	switch (type->type_kind)
	{
		case ALL_SIGNED_INTS:
			if (type->builtin.bitsize < platform_target.width_c_int) return type_cint;
			return type;
		case ALL_UNSIGNED_INTS:
			if (type->builtin.bitsize < platform_target.width_c_int) return type_cuint;
			return type;
		case TYPE_F16:
			// Promote F16 to a real type.
			return type_float;
		case TYPE_OPTIONAL:
			UNREACHABLE
		default:
			return type;
	}
}

bool cast_decay_array_pointers(SemaContext *context, Expr *expr)
{
	CanonicalType *pointer_type = type_pointer_type(type_no_optional(expr->type));
	if (!pointer_type || !type_is_arraylike(pointer_type)) return true;
	return cast_implicit(context, expr, type_add_optional(type_get_ptr(pointer_type->array.base), IS_OPTIONAL(expr)));
}

void cast_to_max_bit_size(SemaContext *context, Expr *left, Expr *right, Type *left_type, Type *right_type)
{
	unsigned bit_size_left = left_type->builtin.bitsize;
	unsigned bit_size_right = right_type->builtin.bitsize;
	assert(bit_size_left && bit_size_right);
	if (bit_size_left == bit_size_right) return;
	if (bit_size_left < bit_size_right)
	{
		Type *to = left->type->type_kind < TYPE_U8
		           ? type_int_signed_by_bitsize(bit_size_right)
		           : type_int_unsigned_by_bitsize(bit_size_right);
		bool success = cast_implicit(context, left, to);
		assert(success);
		return;
	}
	Type *to = right->type->type_kind < TYPE_U8
	           ? type_int_signed_by_bitsize(bit_size_left)
	           : type_int_unsigned_by_bitsize(bit_size_left);
	bool success = cast_implicit(context, right, to);
	assert(success);
}


#pragma clang diagnostic pop