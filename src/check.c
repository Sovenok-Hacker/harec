#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "check.h"
#include "eval.h"
#include "expr.h"
#include "scope.h"
#include "trace.h"
#include "type_store.h"
#include "types.h"
#include "util.h"

static void
mkident(struct context *ctx, struct identifier *out, const struct identifier *in)
{
	identifier_dup(out, in);
	if (ctx->ns) {
		out->ns = xcalloc(1, sizeof(struct identifier));
		identifier_dup(out->ns, ctx->ns);
	}
}

static void
expect(const struct location *loc, bool constraint, char *fmt, ...)
{
	if (!constraint) {
		va_list ap;
		va_start(ap, fmt);

		fprintf(stderr, "Error %s:%d:%d: ",
			loc->path, loc->lineno, loc->colno);
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
		abort();
	}
}

static struct expression *
lower_implicit_cast(const struct type *to, struct expression *expr)
{
	if (to == expr->result) {
		return expr;
	}
	struct expression *cast = xcalloc(1, sizeof(struct expression));
	cast->type = EXPR_CAST;
	cast->result = to;
	cast->terminates = expr->terminates;
	cast->cast.kind = C_CAST;
	cast->cast.value = expr;
	return cast;
}

void check_expression(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *type);

static void
check_expr_access(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trace(TR_CHECK, "access");
	expr->type = EXPR_ACCESS;
	expr->access.type = aexpr->access.type;

	const struct scope_object *obj;
	switch (expr->access.type) {
	case ACCESS_IDENTIFIER:
		obj = scope_lookup(ctx->scope, &aexpr->access.ident);
		char buf[1024];
		identifier_unparse_static(&aexpr->access.ident, buf, sizeof(buf));
		expect(&aexpr->loc, obj, "Unknown object '%s'", buf);
		switch (obj->otype) {
		case O_CONST:
			// Lower constants
			*expr = *obj->value;
			break;
		case O_BIND:
		case O_DECL:
			expr->result = obj->type;
			expr->access.object = obj;
			break;
		case O_TYPE:
			expect(&aexpr->loc, false, "Expected identifier, got type");
		}
		break;
	case ACCESS_INDEX:
		expr->access.array = xcalloc(1, sizeof(struct expression));
		expr->access.index = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->access.array,
			expr->access.array, NULL);
		check_expression(ctx, aexpr->access.index,
			expr->access.index, NULL);
		const struct type *atype =
			type_dereference(expr->access.array->result);
		expect(&aexpr->access.array->loc, atype,
			"Cannot dereference nullable pointer for indexing");
		const struct type *itype =
			type_dealias(expr->access.index->result);
		expect(&aexpr->access.array->loc,
			atype->storage == TYPE_STORAGE_ARRAY || atype->storage == TYPE_STORAGE_SLICE,
			"Cannot index non-array, non-slice %s object",
			type_storage_unparse(atype->storage));
		expect(&aexpr->access.index->loc, type_is_integer(itype),
			"Cannot use non-integer %s type as slice/array index",
			type_storage_unparse(itype->storage));
		expr->access.index = lower_implicit_cast(
			&builtin_type_size, expr->access.index);
		expr->result = type_store_lookup_with_flags(&ctx->store,
			atype->array.members, atype->flags | atype->array.members->flags);
		break;
	case ACCESS_FIELD:
		expr->access._struct = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->access._struct,
			expr->access._struct, NULL);
		const struct type *stype =
			type_dereference(expr->access._struct->result);
		expect(&aexpr->access._struct->loc, stype,
			"Cannot dereference nullable pointer for field selection");
		expect(&aexpr->access._struct->loc,
			stype->storage == TYPE_STORAGE_STRUCT || stype->storage == TYPE_STORAGE_UNION,
			"Cannot index non-struct, non-union object");
		expr->access.field = type_get_field(stype, aexpr->access.field);
		expect(&aexpr->access._struct->loc, expr->access.field,
			"No such struct field '%s'", aexpr->access.field);
		expr->result = expr->access.field->type;
		break;
	}
}

static void
check_expr_assert(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trace(TR_CHECK, "assert");
	expr->type = EXPR_ASSERT;
	expr->result = &builtin_type_void;

	if (aexpr->assert.cond != NULL) {
		expr->assert.cond = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->assert.cond,
			expr->assert.cond, &builtin_type_bool);
		expect(&aexpr->assert.cond->loc,
			expr->assert.cond->result->storage == TYPE_STORAGE_BOOL,
			"Assertion condition must be boolean");
	} else {
		expr->terminates = true;
	}

	expr->assert.message = xcalloc(1, sizeof(struct expression));
	if (aexpr->assert.message != NULL) {
		check_expression(ctx, aexpr->assert.message,
			expr->assert.message, &builtin_type_str);
		expect(&aexpr->assert.message->loc,
			expr->assert.message->result->storage == TYPE_STORAGE_STRING,
			"Assertion message must be string");
	} else {
		int n = snprintf(NULL, 0, "Assertion failed: %s:%d:%d",
			aexpr->loc.path, aexpr->loc.lineno, aexpr->loc.colno);
		char *s = xcalloc(1, n + 1);
		snprintf(s, n, "Assertion failed: %s:%d:%d",
			aexpr->loc.path, aexpr->loc.lineno, aexpr->loc.colno);

		expr->assert.message->type = EXPR_CONSTANT;
		expr->assert.message->result = &builtin_type_const_str;
		expr->assert.message->constant.string.value = s;
		expr->assert.message->constant.string.len = n;
	}
}

static void
check_expr_assign(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trace(TR_CHECK, "assign");
	expr->type = EXPR_ASSIGN;
	expr->result = &builtin_type_void;
	expr->assign.indirect = aexpr->assign.indirect;
	struct expression *object = xcalloc(1, sizeof(struct expression));
	struct expression *value = xcalloc(1, sizeof(struct expression));

	check_expression(ctx, aexpr->assign.object, object, NULL);
	check_expression(ctx, aexpr->assign.value, value, object->result);

	expr->assign.op = aexpr->assign.op;

	if (aexpr->assign.indirect) {
		expect(&aexpr->loc,
			object->result->storage == TYPE_STORAGE_POINTER,
			"Cannot dereference non-pointer type for assignment");
		expect(&aexpr->loc,
			!(object->result->pointer.flags & PTR_NULLABLE),
			"Cannot dereference nullable pointer type");
		expect(&aexpr->loc,
			type_is_assignable(&ctx->store,
				object->result->pointer.referent,
				value->result),
			"Value type is not assignable to pointer type");
		value = lower_implicit_cast(object->result->pointer.referent, value);
	} else {
		assert(object->type == EXPR_ACCESS); // Invariant
		expect(&aexpr->loc, !(object->result->flags & TYPE_CONST),
				"Cannot assign to const object");
		expect(&aexpr->loc,
			type_is_assignable(&ctx->store, object->result, value->result),
			"rvalue type is not assignable to lvalue");
		value = lower_implicit_cast(object->result, value);
	}

	expr->assign.object = object;
	expr->assign.value = value;
}

static void
check_expr_binarithm(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trace(TR_CHECK, "binarithm");
	expr->type = EXPR_BINARITHM;
	expr->binarithm.op = aexpr->binarithm.op;

	struct expression *lvalue = xcalloc(1, sizeof(struct expression)),
		*rvalue = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->binarithm.lvalue, lvalue, NULL);
	check_expression(ctx, aexpr->binarithm.rvalue, rvalue, NULL);
	expr->binarithm.lvalue = lvalue;
	expr->binarithm.rvalue = rvalue;

	switch (expr->binarithm.op) {
	// Numeric arithmetic
	case BIN_BAND:
	case BIN_BOR:
	case BIN_DIV:
	case BIN_LSHIFT:
	case BIN_MINUS:
	case BIN_MODULO:
	case BIN_PLUS:
	case BIN_RSHIFT:
	case BIN_TIMES:
	case BIN_BXOR:
		// TODO: Promotion
		assert(lvalue->result->storage == rvalue->result->storage);
		expr->result = lvalue->result;
		break;
	// Logical arithmetic
	case BIN_GREATER:
	case BIN_GREATEREQ:
	case BIN_LAND:
	case BIN_LEQUAL:
	case BIN_LESS:
	case BIN_LESSEQ:
	case BIN_LOR:
	case BIN_LXOR:
	case BIN_NEQUAL:
		// TODO: Promotion, comparibility rules
		assert(lvalue->result->storage == rvalue->result->storage);
		expr->result = &builtin_type_bool;
		break;
	}
}

static void
check_expr_binding(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trace(TR_CHECK, "binding");
	expr->type = EXPR_BINDING;
	expr->result = &builtin_type_void;

	struct expression_binding *binding = &expr->binding;
	struct expression_binding **next = &expr->binding.next;

	const struct ast_expression_binding *abinding = &aexpr->binding;
	while (abinding) {
		const struct type *type = NULL;
		if (abinding->type) {
			type = type_store_lookup_atype(
				&ctx->store, abinding->type);
			type = type_store_lookup_with_flags(&ctx->store,
				type, type->flags | abinding->flags);
		}

		struct identifier ident = {
			.name = abinding->name,
		};
		struct expression *initializer =
			xcalloc(1, sizeof(struct expression));
		check_expression(ctx, abinding->initializer, initializer, type);

		if (!type) {
			type = type_store_lookup_with_flags(&ctx->store,
				initializer->result, abinding->flags);
		}
		expect(&aexpr->loc,
			type->size != 0 && type->size != SIZE_UNDEFINED,
			"Cannot create binding for type of zero or undefined size");
		expect(&aexpr->loc,
			type_is_assignable(&ctx->store, type, initializer->result),
			"Initializer is not assignable to binding type");
		binding->initializer =
			lower_implicit_cast(type, initializer);

		if (!abinding->is_static) {
			binding->object = scope_insert(ctx->scope, O_BIND,
				&ident, &ident, type, NULL);
		} else {
			struct expression *value =
				xcalloc(1, sizeof(struct expression));
			enum eval_result r = eval_expr(ctx, initializer, value);
			expect(&abinding->initializer->loc, r == EVAL_OK,
				"Unable to evaluate static initializer at compile time");
			// TODO: Free initializer
			initializer = value;

			struct identifier gen = {0};
			int n = snprintf(NULL, 0, "static.%d", ctx->id);
			gen.name = xcalloc(n + 1, 1);
			snprintf(gen.name, n + 1, "static.%d", ctx->id);
			++ctx->id;
			binding->object = scope_insert(ctx->scope, O_DECL,
				&gen, &ident, type, NULL);
		}

		if (abinding->next) {
			binding = *next =
				xcalloc(1, sizeof(struct expression_binding));
			next = &binding->next;
		}

		abinding = abinding->next;
	}
}

// Lower Hare-style variadic arguments into an array literal
static void
lower_vaargs(struct context *ctx,
	const struct ast_call_argument *aarg,
	struct expression *vaargs,
	const struct type *type)
{
	struct ast_expression val = {
		.type = EXPR_CONSTANT,
		.loc = aarg->value->loc,
		.constant = {
			.storage = TYPE_STORAGE_ARRAY,
		},
	};
	struct ast_array_constant **next = &val.constant.array;
	while (aarg) {
		struct ast_array_constant *item = *next =
			xcalloc(1, sizeof(struct ast_array_constant));
		item->value = aarg->value;
		aarg = aarg->next;
		next = &item->next;
	}

	// XXX: This error handling is minimum-effort and bad
	const struct type *hint = type_store_lookup_array(
		&ctx->store, type, SIZE_UNDEFINED);
	check_expression(ctx, &val, vaargs, hint);
	assert(vaargs->result->storage == TYPE_STORAGE_ARRAY);
	expect(&val.loc, vaargs->result->array.members == type,
		"Argument is not assignable to variadic parameter type");

	struct ast_array_constant *item = val.constant.array;
	while (item) {
		struct ast_array_constant *next = item->next;
		free(item);
		item = next;
	}
}

static void
check_expr_call(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "call");
	expr->type = EXPR_CALL;

	struct expression *lvalue = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->call.lvalue, lvalue, NULL);
	expr->call.lvalue = lvalue;

	const struct type *fntype = type_dereference(lvalue->result);
	expect(&aexpr->loc, fntype,
		"Cannot dereference nullable pointer type for function call");
	expect(&aexpr->loc,
		fntype->storage == TYPE_STORAGE_FUNCTION,
		"Cannot call non-function type");
	expr->result = fntype->func.result;

	struct call_argument *arg, **next = &expr->call.args;
	struct ast_call_argument *aarg = aexpr->call.args;
	struct type_func_param *param = fntype->func.params;
	while (param && aarg) {
		trenter(TR_CHECK, "arg");
		arg = *next = xcalloc(1, sizeof(struct call_argument));
		arg->value = xcalloc(1, sizeof(struct expression));

		if (!param->next && fntype->func.variadism == VARIADISM_HARE
				&& !aarg->variadic) {
			lower_vaargs(ctx, aarg, arg->value,
				param->type->array.members);
			arg->value = lower_implicit_cast(param->type, arg->value);
			param = NULL;
			aarg = NULL;
			trleave(TR_CHECK, NULL);
			break;
		}

		check_expression(ctx, aarg->value, arg->value, param->type);

		expect(&aarg->value->loc,
			type_is_assignable(&ctx->store,
				param->type, arg->value->result),
			"Argument is not assignable to parameter type");
		arg->value = lower_implicit_cast(param->type, arg->value);

		aarg = aarg->next;
		param = param->next;
		next = &arg->next;
		trleave(TR_CHECK, NULL);
	}

	expect(&aexpr->loc, !aarg, "Too many parameters for function call");
	expect(&aexpr->loc, !param, "Not enough parameters for function call");

	trleave(TR_CHECK, NULL);
}

static void
check_expr_cast(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trace(TR_CHECK, "cast");
	expr->type = EXPR_CAST;
	expr->cast.kind = aexpr->cast.kind;
	struct expression *value = expr->cast.value =
		xcalloc(1, sizeof(struct expression));
	const struct type *secondary = expr->cast.secondary =
		type_store_lookup_atype(&ctx->store, aexpr->cast.type);
	check_expression(ctx, aexpr->cast.value, value, secondary);
	expect(&aexpr->cast.type->loc,
		type_is_castable(secondary, value->result),
		"Invalid cast");

	if (aexpr->cast.kind == C_ASSERTION || aexpr->cast.kind == C_TEST) {
		const struct type *primary = type_dealias(expr->cast.value->result);
		expect(&aexpr->cast.value->loc,
			primary->storage == TYPE_STORAGE_TAGGED_UNION,
			"Expected a tagged union type");
		bool found = false;
		for (const struct type_tagged_union *t = &primary->tagged;
				t; t = t->next) {
			if (t->type->id == secondary->id) {
				found = true;
				break;
			}
		}
		expect(&aexpr->cast.type->loc, found,
			"Type is not a valid member of the tagged union type");
	}

	switch (aexpr->cast.kind) {
	case C_CAST:
	case C_ASSERTION:
		expr->result = secondary;
		break;
	case C_TEST:
		expr->result = &builtin_type_bool;
		break;
	}
}

static void
check_expr_array(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	size_t len = 0;
	bool expandable = false;
	struct ast_array_constant *item = aexpr->constant.array;
	struct array_constant *cur, **next = &expr->constant.array;
	const struct type *type = NULL;
	if (hint) {
		type = hint->array.members;
	}

	while (item) {
		struct expression *value = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, item->value, value, type);
		cur = *next = xcalloc(1, sizeof(struct array_constant));
		cur->value = value;

		if (!type) {
			type = value->result;
		} else {
			expect(&item->value->loc,
				type_is_assignable(&ctx->store, type, value->result),
				"Array members must be of a uniform type");
			cur->value = lower_implicit_cast(type, cur->value);
		}

		if (item->expand) {
			expandable = true;
			expr->constant.array->expand = true;
			assert(!item->next);
		}

		item = item->next;
		next = &cur->next;
		++len;
	}

	if (expandable) {
		expect(&aexpr->loc, hint != NULL,
			"Cannot expand array for inferred type");
		expect(&aexpr->loc, hint->storage == TYPE_STORAGE_ARRAY
				&& hint->array.length != SIZE_UNDEFINED
				&& hint->array.length >= len,
			"Cannot expand array into destination type");
		expr->result = type_store_lookup_array(&ctx->store,
				type, hint->array.length);
	} else {
		expr->result = type_store_lookup_array(&ctx->store, type, len);
	}
}

static void
check_expr_constant(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trace(TR_CHECK, "constant");
	expr->type = EXPR_CONSTANT;
	expr->result = builtin_type_for_storage(aexpr->constant.storage, false);

	switch (aexpr->constant.storage) {
	case TYPE_STORAGE_I8:
	case TYPE_STORAGE_I16:
	case TYPE_STORAGE_I32:
	case TYPE_STORAGE_I64:
	case TYPE_STORAGE_INT:
		expr->constant.ival = aexpr->constant.ival;
		break;
	case TYPE_STORAGE_U8:
	case TYPE_STORAGE_U16:
	case TYPE_STORAGE_U32:
	case TYPE_STORAGE_U64:
	case TYPE_STORAGE_UINT:
	case TYPE_STORAGE_SIZE:
		expr->constant.uval = aexpr->constant.uval;
		break;
	case TYPE_STORAGE_RUNE:
		expr->constant.rune = aexpr->constant.rune;
		break;
	case TYPE_STORAGE_BOOL:
		expr->constant.bval = aexpr->constant.bval;
		break;
	case TYPE_STORAGE_NULL:
	case TYPE_STORAGE_VOID:
		// No storage
		break;
	case TYPE_STORAGE_ARRAY:
		check_expr_array(ctx, aexpr, expr, hint);
		break;
	case TYPE_STORAGE_STRING:
		expr->constant.string.len = aexpr->constant.string.len;
		expr->constant.string.value = xcalloc(1, aexpr->constant.string.len);
		memcpy(expr->constant.string.value, aexpr->constant.string.value,
			aexpr->constant.string.len);
		break;
	case TYPE_STORAGE_F32:
	case TYPE_STORAGE_F64:
	case TYPE_STORAGE_STRUCT:
		assert(0); // TODO
	case TYPE_STORAGE_CHAR:
	case TYPE_STORAGE_ENUM:
	case TYPE_STORAGE_UINTPTR:
	case TYPE_STORAGE_ALIAS:
	case TYPE_STORAGE_FUNCTION:
	case TYPE_STORAGE_POINTER:
	case TYPE_STORAGE_SLICE:
	case TYPE_STORAGE_TAGGED_UNION:
	case TYPE_STORAGE_UNION:
		assert(0); // Invariant
	}
}

static void
check_expr_defer(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	expect(&aexpr->loc, !ctx->deferring,
		"Cannot defer within another defer expression.");
	expr->type = EXPR_DEFER;
	expr->result = &builtin_type_void;
	expr->defer.deferred = xcalloc(1, sizeof(struct expression));
	ctx->deferring = true;
	check_expression(ctx, aexpr->defer.deferred, expr->defer.deferred, NULL);
	ctx->deferring = false;
}

static void
check_expr_control(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "control");
	expr->type = aexpr->type;
	expr->terminates = true;
	char *label = expr->control.label = aexpr->control.label;

	struct scope *scope = ctx->scope;
	for (; scope != NULL; scope = scope->parent) {
		if (scope->type != EXPR_FOR) {
			continue;
		}
		if (label == NULL) {
			break;
		}
		if (scope->label != NULL && strcmp(label, scope->label) == 0) {
			break;
		}
	}
	expect(&aexpr->loc, scope != NULL, "Unknown label %s",
		expr->control.label);
	trleave(TR_CHECK, NULL);
}

static void
check_expr_for(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "if");
	expr->type = EXPR_FOR;
	expr->result = &builtin_type_void;

	if (aexpr->_for.label) {
		expr->_for.label = strdup(aexpr->_for.label);
	}

	struct scope *scope = scope_push(&ctx->scope, TR_CHECK);
	expr->_for.scope = scope;
	scope->type = expr->type;
	scope->label = expr->_for.label;
	if (expr->_for.label) {
		for (scope = scope->parent; scope; scope = scope->parent) {
			if (scope->label == NULL) {
				continue;
			}
			expect(&aexpr->_for.label_loc,
				strcmp(scope->label, expr->_for.label) != 0,
				"for loop label must be unique among its ancestors");
		}
	}

	struct expression *bindings = NULL,
		*cond = NULL, *afterthought = NULL, *body = NULL;

	if (aexpr->_for.bindings) {
		bindings = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->_for.bindings, bindings, NULL);
		expr->_for.bindings = bindings;
	}

	cond = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->_for.cond, cond, &builtin_type_bool);
	expr->_for.cond = cond;
	expect(&aexpr->_for.cond->loc,
		cond->result->storage == TYPE_STORAGE_BOOL,
		"Expected for condition to be boolean");

	if (aexpr->_for.afterthought) {
		afterthought = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->_for.afterthought,
			afterthought, NULL);
		expr->_for.afterthought = afterthought;
	}

	body = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->_for.body, body, NULL);
	expr->_for.body = body;

	scope_pop(&ctx->scope, TR_CHECK);
	trleave(TR_CHECK, NULL);
}

static void
check_expr_if(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "if");
	expr->type = EXPR_IF;

	struct expression *cond, *true_branch, *false_branch = NULL;

	cond = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->_if.cond, cond, &builtin_type_bool);

	true_branch = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->_if.true_branch, true_branch, NULL);

	if (aexpr->_if.false_branch) {
		false_branch = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->_if.false_branch,
				false_branch, NULL);

		if (true_branch->terminates && false_branch->terminates) {
			expr->result = &builtin_type_void;
		} else if (true_branch->terminates) {
			expr->result = false_branch->result;
		} else if (false_branch->terminates) {
			expr->result = true_branch->result;
		} else {
			// TODO: Tagged unions
			assert(true_branch->result == false_branch->result);
			expr->result = true_branch->result;
		}
	} else {
		expr->result = &builtin_type_void;
		expr->terminates = true_branch->terminates;
	}

	expect(&aexpr->_if.cond->loc,
		cond->result->storage == TYPE_STORAGE_BOOL,
		"Expected if condition to be boolean");

	expr->_if.cond = cond;
	expr->_if.true_branch = true_branch;
	expr->_if.false_branch = false_branch;

	trleave(TR_CHECK, NULL);
}

static void
check_expr_list(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "expression-list");
	expr->type = EXPR_LIST;

	struct scope *scope = scope_push(&ctx->scope, TR_CHECK);
	expr->list.scope = scope;
	scope->type = expr->type;

	struct expressions *list = &expr->list.exprs;
	struct expressions **next = &list->next;

	const struct ast_expression_list *alist = &aexpr->list;
	while (alist) {
		struct expression *lexpr = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, alist->expr, lexpr, NULL);
		list->expr = lexpr;

		alist = alist->next;
		if (alist) {
			*next = xcalloc(1, sizeof(struct expressions));
			list = *next;
			next = &list->next;
		} else {
			expr->result = lexpr->result;
			expr->terminates = lexpr->terminates;
		}
	}

	scope_pop(&ctx->scope, TR_CHECK);
	trleave(TR_CHECK, NULL);
}

static void
check_expr_measure(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "measure");
	expr->type = EXPR_MEASURE;
	expr->result = &builtin_type_size;
	expr->measure.op = aexpr->measure.op;

	switch (expr->measure.op) {
	case M_LEN:
		expr->measure.value = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->measure.value,
			expr->measure.value, NULL);
		enum type_storage vstor = expr->measure.value->result->storage;
		expect(&aexpr->measure.value->loc,
			vstor == TYPE_STORAGE_ARRAY
				|| vstor == TYPE_STORAGE_SLICE
				|| vstor == TYPE_STORAGE_STRING,
			"len argument must be of an array, slice, or str type");
		expect(&aexpr->measure.value->loc,
			expr->measure.value->result->size != SIZE_UNDEFINED,
			"Cannot take length of array type with undefined length");
		break;
	case M_SIZE:
		expr->measure.type = type_store_lookup_atype(
			&ctx->store, aexpr->measure.type);
		break;
	case M_OFFSET:
		assert(0); // TODO
	}
}

static void
check_expr_return(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "return");
	expr->type = EXPR_RETURN;
	expr->result = &builtin_type_void;
	expr->terminates = true;

	if (aexpr->_return.value) {
		struct expression *rval = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->_return.value,
			rval, ctx->current_fntype->func.result);
		expect(&aexpr->_return.value->loc,
			type_is_assignable(&ctx->store, ctx->current_fntype->func.result, rval->result),
			"Return value is not assignable to function result type");
		if (ctx->current_fntype->func.result != rval->result) {
			rval = lower_implicit_cast(
				ctx->current_fntype->func.result, rval);
		}
		expr->_return.value = rval;
	}

	trleave(TR_CHECK, NULL);
}

static void
check_expr_slice(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "slice");
	expr->type = EXPR_SLICE;

	expr->slice.object = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->slice.object, expr->slice.object, NULL);
	const struct type *atype =
		type_dereference(expr->slice.object->result);
	expect(&aexpr->slice.object->loc, atype,
		"Cannot dereference nullable pointer for slicing");
	expect(&aexpr->slice.object->loc,
		expr->slice.object->result->storage == TYPE_STORAGE_SLICE ||
		expr->slice.object->result->storage == TYPE_STORAGE_ARRAY,
		"Cannot slice non-array, non-slice object");

	const struct type *itype;
	if (aexpr->slice.start) {
		expr->slice.start = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->slice.start, expr->slice.start, NULL);
		itype = type_dealias(expr->slice.start->result);
		expect(&aexpr->slice.start->loc, type_is_integer(itype),
			"Cannot use non-integer %s type as slicing operand",
			type_storage_unparse(itype->storage));
		expr->slice.start = lower_implicit_cast(
			&builtin_type_size, expr->slice.start);
	}

	if (aexpr->slice.end) {
		expr->slice.end = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, aexpr->slice.end, expr->slice.end, NULL);
		itype = type_dealias(expr->slice.end->result);
		expect(&aexpr->slice.end->loc, type_is_integer(itype),
			"Cannot use non-integer %s type as slicing operand",
			type_storage_unparse(itype->storage));
		expr->slice.end = lower_implicit_cast(
			&builtin_type_size, expr->slice.end);
	} else {
		// TODO: Assert that array type has a well-defined length
	}

	expr->result = type_store_lookup_slice(&ctx->store,
		expr->slice.object->result->array.members);

	trleave(TR_CHECK, NULL);
}

static void
check_expr_struct(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "struct");
	assert(!aexpr->_struct.autofill); // TODO
	assert(!aexpr->_struct.type.name); // TODO
	expr->type = EXPR_STRUCT;

	struct ast_type stype = {
		.storage = TYPE_STORAGE_STRUCT,
		.flags = TYPE_CONST,
	};
	struct ast_struct_union_type *tfield = &stype.struct_union;
	struct ast_struct_union_type **tnext = &tfield->next;
	struct expression_struct *sexpr = &expr->_struct;
	struct expression_struct **snext = &sexpr->next;

	struct ast_field_value *afield = aexpr->_struct.fields;
	while (afield) {
		assert(!afield->is_embedded); // TODO

		tfield->member_type = MEMBER_TYPE_FIELD;
		tfield->field.name = afield->field.name;
		tfield->field.type = afield->field.type;
		sexpr->value = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, afield->field.initializer, sexpr->value,
			type_store_lookup_atype(&ctx->store, tfield->field.type));

		if (afield->next) {
			*tnext = tfield = xcalloc(
				1, sizeof(struct ast_struct_union_type));
			tnext = &tfield->next;
			*snext = sexpr = xcalloc(
				1, sizeof(struct expression_struct));
			snext = &sexpr->next;
		}

		afield = afield->next;
	}

	expr->result = type_store_lookup_atype(&ctx->store, &stype);

	tfield = &stype.struct_union;
	sexpr = &expr->_struct;
	while (tfield) {
		const struct struct_field *field = type_get_field(
			expr->result, tfield->field.name);
		// TODO: Use more specific error location
		expect(&aexpr->loc, field, "No field by this name exists for this type");
		expect(&aexpr->loc,
			type_is_assignable(&ctx->store, field->type, sexpr->value->result),
			"Cannot initialize struct field from value of this type");
		sexpr->field = field;
		sexpr->value = lower_implicit_cast(field->type, sexpr->value);

		struct ast_struct_union_type *next = tfield->next;
		if (tfield != &stype.struct_union) {
			free(tfield);
		}
		tfield = next;
		sexpr = sexpr->next;
	}

	trleave(TR_CHECK, NULL);
}

static void
check_expr_switch(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "switch");
	expr->type = EXPR_SWITCH;

	struct expression *value = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->_switch.value, value, NULL);
	const struct type *type = value->result;
	expr->_switch.value = value;

	// TODO: Test for dupes, exhaustiveness
	struct switch_case **next = &expr->_switch.cases, *_case = NULL;
	for (struct ast_switch_case *acase = aexpr->_switch.cases;
			acase; acase = acase->next) {
		_case = *next = xcalloc(1, sizeof(struct switch_case));
		next = &_case->next;

		struct case_option *opt, **next_opt = &_case->options;
		for (struct ast_case_option *aopt = acase->options;
				aopt; aopt = aopt->next) {
			opt = *next_opt = xcalloc(1, sizeof(struct case_option));
			struct expression *value =
				xcalloc(1, sizeof(struct expression));
			struct expression *evaled =
				xcalloc(1, sizeof(struct expression));

			check_expression(ctx, aopt->value, value, type);
			// XXX: Should this be assignable instead?
			expect(&aopt->value->loc,
				type == value->result,
				"Invalid type for switch case");

			enum eval_result r = eval_expr(ctx, value, evaled);
			expect(&aopt->value->loc,
				r == EVAL_OK,
				"Unable to evaluate case at compile time");

			opt->value = evaled;
			next_opt = &opt->next;
		}

		_case->value = xcalloc(1, sizeof(struct expression));
		check_expression(ctx, acase->value, _case->value, type);
		if (_case->value->terminates) {
			continue;
		}

		if (expr->result == NULL) {
			expr->result = _case->value->result;
		} else if (expr->result != _case->value->result) {
			assert(0); // TODO: Form tagged union
		}
	}

	if (expr->result == NULL) {
		expr->result = &builtin_type_void;
		expr->terminates = true;
	}
}

static void
check_expr_unarithm(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "unarithm");
	expr->type = EXPR_UNARITHM;

	struct expression *operand = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, aexpr->unarithm.operand, operand, NULL);
	expr->unarithm.operand = operand;
	expr->unarithm.op = aexpr->unarithm.op;

	switch (expr->unarithm.op) {
	case UN_LNOT:
		expect(&aexpr->unarithm.operand->loc,
			operand->result->storage == TYPE_STORAGE_BOOL,
			"Cannot perform logical NOT (!) on non-boolean type");
		expr->result = &builtin_type_bool;
		break;
	case UN_BNOT:
		expect(&aexpr->unarithm.operand->loc,
			type_is_integer(operand->result),
			"Cannot perform binary NOT (~) on non-integer type");
		expect(&aexpr->unarithm.operand->loc,
			!type_is_signed(operand->result),
			"Cannot perform binary NOT (~) on signed type");
		expr->result = operand->result;
		break;
	case UN_MINUS:
	case UN_PLUS:
		expect(&aexpr->unarithm.operand->loc,
			type_is_numeric(operand->result),
			"Cannot perform operation on non-numeric type");
		expect(&aexpr->unarithm.operand->loc,
			type_is_signed(operand->result),
			"Cannot perform operation on unsigned type");
		expr->result = operand->result;
		break;
	case UN_ADDRESS:
		expr->result = type_store_lookup_pointer(
			&ctx->store, operand->result, 0);
		break;
	case UN_DEREF:
		expect(&aexpr->unarithm.operand->loc,
			operand->result->storage == TYPE_STORAGE_POINTER,
			"Cannot de-reference non-pointer type");
		expect(&aexpr->unarithm.operand->loc,
			!(operand->result->pointer.flags & PTR_NULLABLE),
			"Cannot dereference nullable pointer type");
		expr->result = operand->result->pointer.referent;
		break;
	}

	trleave(TR_CHECK, NULL);
}

void
check_expression(struct context *ctx,
	const struct ast_expression *aexpr,
	struct expression *expr,
	const struct type *hint)
{
	trenter(TR_CHECK, "expression");

	switch (aexpr->type) {
	case EXPR_ACCESS:
		check_expr_access(ctx, aexpr, expr, hint);
		break;
	case EXPR_ASSERT:
		check_expr_assert(ctx, aexpr, expr, hint);
		break;
	case EXPR_ASSIGN:
		check_expr_assign(ctx, aexpr, expr, hint);
		break;
	case EXPR_BINARITHM:
		check_expr_binarithm(ctx, aexpr, expr, hint);
		break;
	case EXPR_BINDING:
		check_expr_binding(ctx, aexpr, expr, hint);
		break;
	case EXPR_BREAK:
	case EXPR_CONTINUE:
		check_expr_control(ctx, aexpr, expr, hint);
		break;
	case EXPR_CALL:
		check_expr_call(ctx, aexpr, expr, hint);
		break;
	case EXPR_CAST:
		check_expr_cast(ctx, aexpr, expr, hint);
		break;
	case EXPR_CONSTANT:
		check_expr_constant(ctx, aexpr, expr, hint);
		break;
	case EXPR_DEFER:
		check_expr_defer(ctx, aexpr, expr, hint);
		break;
	case EXPR_FOR:
		check_expr_for(ctx, aexpr, expr, hint);
		break;
	case EXPR_IF:
		check_expr_if(ctx, aexpr, expr, hint);
		break;
	case EXPR_LIST:
		check_expr_list(ctx, aexpr, expr, hint);
		break;
	case EXPR_MATCH:
		assert(0); // TODO
	case EXPR_MEASURE:
		check_expr_measure(ctx, aexpr, expr, hint);
		break;
	case EXPR_RETURN:
		check_expr_return(ctx, aexpr, expr, hint);
		break;
	case EXPR_SLICE:
		check_expr_slice(ctx, aexpr, expr, hint);
		break;
	case EXPR_STRUCT:
		check_expr_struct(ctx, aexpr, expr, hint);
		break;
	case EXPR_SWITCH:
		check_expr_switch(ctx, aexpr, expr, hint);
		break;
	case EXPR_UNARITHM:
		check_expr_unarithm(ctx, aexpr, expr, hint);
		break;
	}

	trleave(TR_CHECK, NULL);
}

static struct declaration *
check_function(struct context *ctx,
	const struct ast_decl *adecl)
{
	if (!adecl->function.body) {
		return NULL; // Prototype
	}

	const struct ast_function_decl *afndecl = &adecl->function;
	trenter(TR_CHECK, "function");

	const struct ast_type fn_atype = {
		.storage = TYPE_STORAGE_FUNCTION,
		.flags = TYPE_CONST,
		.func = afndecl->prototype,
	};
	const struct type *fntype = type_store_lookup_atype(
			&ctx->store, &fn_atype);
	assert(fntype); // Invariant
	ctx->current_fntype = fntype;

	expect(&adecl->loc,
		fntype->func.variadism != VARIADISM_C,
		"C-style variadism is not allowed for function declarations");

	struct declaration *decl = xcalloc(1, sizeof(struct declaration));
	decl->type = DECL_FUNC;
	decl->func.type = fntype;
	decl->func.flags = afndecl->flags;

	if (afndecl->symbol) {
		decl->ident.name = strdup(afndecl->symbol);
		decl->symbol = strdup(afndecl->symbol);
	} else {
		mkident(ctx, &decl->ident, &afndecl->ident);
	}

	decl->func.scope = scope_push(&ctx->scope, TR_CHECK);
	struct ast_function_parameters *params = afndecl->prototype.params;
	while (params) {
		expect(&params->loc, params->name,
			"Function parameters must be named");
		struct identifier ident = {
			.name = params->name,
		};
		const struct type *type = type_store_lookup_atype(
				&ctx->store, params->type);
		if (fntype->func.variadism == VARIADISM_HARE
				&& !params->next) {
			type = type_store_lookup_slice(&ctx->store, type);
		}
		scope_insert(decl->func.scope, O_BIND,
			&ident, &ident, type, NULL);
		params = params->next;
	}

	struct expression *body = xcalloc(1, sizeof(struct expression));
	check_expression(ctx, afndecl->body, body, fntype->func.result);

	expect(&afndecl->body->loc,
		body->terminates || type_is_assignable(&ctx->store, fntype->func.result, body->result),
		"Result value is not assignable to function result type");
	if (!body->terminates && fntype->func.result != body->result) {
		body = lower_implicit_cast(fntype->func.result, body);
	}
	decl->func.body = body;

	// TODO: Add function name to errors
	if ((decl->func.flags & FN_INIT)
			|| (decl->func.flags & FN_FINI)
			|| (decl->func.flags & FN_TEST)) {
		const char *flags = "@flags"; // TODO: Unparse flags
		expect(&adecl->loc, fntype->func.result == &builtin_type_void,
				"%s function must return void", flags);
		expect(&adecl->loc, !decl->exported,
				"%s function cannot be exported", flags);
	}

	scope_pop(&ctx->scope, TR_CHECK);
	ctx->current_fntype = NULL;
	trleave(TR_CHECK, NULL);
	return decl;
}

static struct declaration *
check_global(struct context *ctx,
	const struct ast_decl *adecl)
{
	const struct ast_global_decl *agdecl = &adecl->global;
	if (!agdecl->init) {
		return NULL; // Forward declaration
	}

	const struct type *type = type_store_lookup_atype(
			&ctx->store, agdecl->type);

	// TODO: Free initialier
	struct expression *initializer =
		xcalloc(1, sizeof(struct expression));
	check_expression(ctx, agdecl->init, initializer, type);

	expect(&agdecl->init->loc,
		type_is_assignable(&ctx->store, type, initializer->result),
		"Constant type is not assignable from initializer type");
	initializer = lower_implicit_cast(type, initializer);

	struct expression *value =
		xcalloc(1, sizeof(struct expression));
	enum eval_result r = eval_expr(ctx, initializer, value);
	expect(&agdecl->init->loc, r == EVAL_OK,
		"Unable to evaluate global initializer at compile time");

	struct declaration *decl = xcalloc(1, sizeof(struct declaration));
	decl->type = DECL_GLOBAL;
	decl->global.type = type;
	decl->global.value = value;

	if (agdecl->symbol) {
		decl->ident.name = strdup(agdecl->symbol);
		decl->symbol = strdup(agdecl->symbol);
	} else {
		mkident(ctx, &decl->ident, &agdecl->ident);
	}

	return decl;
}

static struct declaration *
check_type(struct context *ctx,
	const struct ast_decl *adecl)
{
	const struct type *type =
		type_store_lookup_atype(&ctx->store, adecl->type.type);
	struct declaration *decl = xcalloc(1, sizeof(struct declaration));
	decl->type = DECL_TYPE;
	decl->_type = type;
	mkident(ctx, &decl->ident, &adecl->type.ident);
	return decl;
}

static struct declarations **
check_declarations(struct context *ctx,
		const struct ast_decls *adecls,
		struct declarations **next)
{
	trenter(TR_CHECK, "declarations");
	while (adecls) {
		struct declaration *decl = NULL;
		const struct ast_decl *adecl = &adecls->decl;
		switch (adecl->decl_type) {
		case AST_DECL_CONST:
			break; // Handled in scan
		case AST_DECL_FUNC:
			decl = check_function(ctx, adecl);
			break;
		case AST_DECL_GLOBAL:
			decl = check_global(ctx, adecl);
			break;
		case AST_DECL_TYPE:
			decl = check_type(ctx, adecl);
			break;
		}

		if (decl) {
			struct declarations *decls = *next =
				xcalloc(1, sizeof(struct declarations));
			decl->exported = adecl->exported;
			decls->decl = decl;
			next = &decls->next;
		}

		adecls = adecls->next;
	}
	trleave(TR_CHECK, NULL);
	return next;
}

static void
scan_const(struct context *ctx, const struct ast_global_decl *decl)
{
	trenter(TR_SCAN, "constant");
	assert(!decl->symbol); // Invariant

	const struct type *type = type_store_lookup_atype(
			&ctx->store, decl->type);
	// TODO:
	// - Free the initializer
	// - Defer if we can't evaluate it now (for forward references)
	struct expression *initializer =
		xcalloc(1, sizeof(struct expression));
	check_expression(ctx, decl->init, initializer, type);

	expect(&decl->init->loc, type_is_assignable(&ctx->store, type, initializer->result),
		"Constant type is not assignable from initializer type");
	initializer = lower_implicit_cast(type, initializer);

	struct expression *value =
		xcalloc(1, sizeof(struct expression));
	enum eval_result r = eval_expr(ctx, initializer, value);
	// TODO: More forward reference issues:
	expect(&decl->init->loc, r == EVAL_OK,
		"Unable to evaluate constant initializer at compile time");

	struct identifier ident = {0};
	mkident(ctx, &ident, &decl->ident);
	scope_insert(ctx->unit, O_CONST, &ident, &decl->ident, type, value);

	trleave(TR_SCAN, NULL);
}

static void
scan_function(struct context *ctx, const struct ast_function_decl *decl)
{
	trenter(TR_SCAN, "function");
	const struct ast_type fn_atype = {
		.storage = TYPE_STORAGE_FUNCTION,
		.flags = TYPE_CONST,
		.func = decl->prototype,
	};
	const struct type *fntype = type_store_lookup_atype(
			&ctx->store, &fn_atype);
	assert(fntype); // TODO: Forward references

	struct identifier ident = {0};
	if (decl->symbol) {
		ident.name = strdup(decl->symbol);
	} else {
		mkident(ctx, &ident, &decl->ident);
	}
	scope_insert(ctx->unit, O_DECL, &ident, &decl->ident, fntype, NULL);

	char buf[1024];
	identifier_unparse_static(&decl->ident, buf, sizeof(buf));
	trleave(TR_SCAN, "func %s", buf);
}

static void
scan_global(struct context *ctx, const struct ast_global_decl *decl)
{
	trenter(TR_SCAN, "global");

	const struct type *type = type_store_lookup_atype(
			&ctx->store, decl->type);
	assert(type); // TODO: Forward references

	struct identifier ident = {0};
	if (decl->symbol) {
		ident.name = strdup(decl->symbol);
	} else {
		mkident(ctx, &ident, &decl->ident);
	}
	scope_insert(ctx->unit, O_DECL, &ident, &decl->ident, type, NULL);

	trleave(TR_SCAN, NULL);
}

static void
scan_type(struct context *ctx, const struct ast_type_decl *decl)
{
	trenter(TR_SCAN, "type");
	const struct type *type =
		type_store_lookup_atype(&ctx->store, decl->type);

	struct identifier ident = {0};
	mkident(ctx, &ident, &decl->ident);
	scope_insert(ctx->unit, O_TYPE, &ident, &decl->ident, type, NULL);

	if (type->storage == TYPE_STORAGE_ENUM) {
		for (struct type_enum_value *value = type->_enum.values; value;
				value = value->next) {
			struct ast_type atype = {
				.loc = decl->type->loc,
				.storage = TYPE_STORAGE_ALIAS,
				.flags = 0,
				.unwrap = false,
				.alias = decl->ident,
			};
			const struct type *alias =
				type_store_lookup_atype(&ctx->store, &atype);

			struct expression *expr =
				xcalloc(sizeof(struct expression), 1);
			expr->type = EXPR_CONSTANT;
			expr->result = alias;
			if (type_is_signed(alias)) {
				expr->constant.ival = value->ival;
			} else {
				expr->constant.uval = value->uval;
			}

			struct identifier name_ns = {
				.name = decl->ident.name,
				.ns = decl->ident.ns,
			};
			struct identifier name = {
				.name = value->name,
				.ns = &name_ns,
			};
			struct identifier vident = {
				.name = value->name,
				.ns = &ident,
			};
			scope_insert(ctx->unit, O_CONST, &name, &vident, alias, expr);
		}
	}
	trleave(TR_SCAN, NULL);
}

static void
scan_declarations(struct context *ctx, const struct ast_decls *decls)
{
	trenter(TR_SCAN, "declarations");
	while (decls) {
		const struct ast_decl *decl = &decls->decl;
		switch (decl->decl_type) {
		case AST_DECL_CONST:
			scan_const(ctx, &decl->constant);
			break;
		case AST_DECL_FUNC:
			scan_function(ctx, &decl->function);
			break;
		case AST_DECL_GLOBAL:
			scan_global(ctx, &decl->global);
			break;
		case AST_DECL_TYPE:
			scan_type(ctx, &decl->type);
			break;
		}
		decls = decls->next;
	}
	trleave(TR_SCAN, NULL);
}

void
check(struct context *ctx, const struct ast_unit *aunit, struct unit *unit)
{
	builtin_types_init();
	ctx->store.check_context = ctx;
	ctx->ns = unit->ns;

	// Top-level scope management involves:
	//
	// - Creating a top-level scope for the whole unit, to which
	//   declarations are added.
	// - Creating a scope for each sub-unit, and populating it with imports.
	// 
	// Further down the call frame, subsequent functions will create
	// sub-scopes for each declaration, expression-list, etc.
	ctx->unit = scope_push(&ctx->scope, TR_MAX);

	struct scopes *subunit_scopes;
	struct scopes **next = &subunit_scopes;

	// First pass populates the type graph
	for (const struct ast_subunit *su = &aunit->subunits;
			su; su = su->next) {
		scope_push(&ctx->scope, TR_SCAN);

		assert(!su->imports); // TODO
		scan_declarations(ctx, &su->decls);

		*next = xcalloc(1, sizeof(struct scopes));
		(*next)->scope = scope_pop(&ctx->scope, TR_SCAN);
		next = &(*next)->next;
	}

	// Second pass populates the expression graph
	struct scopes *scope = subunit_scopes;
	struct declarations **next_decl = &unit->declarations;
	for (const struct ast_subunit *su = &aunit->subunits;
			su; su = su->next) {
		ctx->scope = scope->scope;
		trenter(TR_CHECK, "scope %p", ctx->scope);
		next_decl = check_declarations(ctx, &su->decls, next_decl);
		trleave(TR_CHECK, NULL);
		scope = scope->next;
	}
}
