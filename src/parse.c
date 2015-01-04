#include "error.h"
#include "ir.h"
#include "token.h"
#include "symbol.h"

#include <stdlib.h>
#include <string.h>

static block_t *declaration(block_t *, const symbol_t **);
static typetree_t *declaration_specifiers();
static typetree_t *declarator(typetree_t *, const char **);
static typetree_t *pointer(const typetree_t *);
static typetree_t *direct_declarator(typetree_t *, const char **);
static typetree_t *parameter_list(const typetree_t *);
static block_t *block(block_t *);
static block_t *statement(block_t *);

static const symbol_t *identifier();

/* expression nodes that are called in high level rules */
static var_t expression(block_t *);
static var_t constant_expression(block_t *);
static var_t assignment_expression(block_t *);

extern int var_stack_offset;

/* External interface */
function_t *
parse()
{
    function_t *fun;
    const symbol_t *symbol;
    block_t *body;

    fun = cfg_create();
    body = block_init();

    do {
        if (peek() == '$')
            break;
        declaration(body, &symbol);
    } while (symbol->type->type != FUNCTION);

    fun->symbol = symbol;
    fun->body = body;

    /* Hack: write stack offset as we are done adding all symbols,
     * reading variable from symtab code. */
    fun->locals_size = (-1) * var_stack_offset;
    return fun;
}

/* Cover both external declarations, functions, and local declarations (with
 * optional initialization code) inside functions. Symbol is bound to last
 * declared identifier. */
static block_t *
declaration(block_t *parent, const symbol_t **symbol)
{
    typetree_t *type, *base;
    int i;

    base = declaration_specifiers();

    while (1) {
        const char *name = NULL;
        type = declarator(base, &name);
        *symbol = sym_add(name, type);

        free((void *) name);

        switch (peek()) {
            case ';':
                consume(';');
                return parent;
            case '=': {
                var_t val;
                consume('=');
                val = assignment_expression(parent);
                if (val.kind == DIRECT) {
                    const typetree_t *newtype = type_combine((*symbol)->type, val.type);
                    if (!newtype) {
                        char *a = typetostr(val.type),
                             *b = typetostr((*symbol)->type);
                        error("Cannot assign value of type `%s` to variable of type `%s`.", a, b);
                        free(a), free(b);
                        exit(1);
                    }
                    ((symbol_t*)*symbol)->type = newtype;
                    ((symbol_t*)*symbol)->value = malloc(sizeof(value_t));
                    ((symbol_t*)*symbol)->value->v_long = val.value.v_long;
                } else if ((*symbol)->depth == 0) {
                    error("Declaration must have constant value.");
                    exit(1);
                }
                eval_assign(parent, var_direct(*symbol), val);
                if (peek() != ',') {
                    consume(';');
                    return parent;
                }
                break;
            }
            case '{':
                if (type->type != FUNCTION || (*symbol)->depth > 0) {
                    error("Invalid function definition.");
                    exit(1);
                }
                push_scope();
                for (i = type->n_args - 1; i >= 0; --i) {
                    if (!type->params[i]) {
                        error("Missing parameter name at position %d", i + 1);
                        exit(1);
                    }
                    sym_add(type->params[i], type->args[i]);
                }
                parent = block(parent); /* generate code */

                pop_scope();
                return parent;
            default:
                break;
        }
        consume(',');
    }
}

static typetree_t *
declaration_specifiers()
{
    int end = 0;
    typetree_t *type = NULL; 
    int flags = 0x0;
    while (1) {
        switch (peek()) {
            case AUTO: case REGISTER: case STATIC: case EXTERN: case TYPEDEF:
                /* todo: something about storage class, maybe do it before this */
                break;
            case CHAR:
                type = type_init(CHAR_T);
                break;
            case SHORT:
            case INT:
            case LONG:
            case SIGNED:
            case UNSIGNED:
                type = type_init(INT64_T);
                break;
            case FLOAT:
            case DOUBLE:
                type = type_init(DOUBLE_T);
                break;
            case VOID:
                type = type_init(VOID_T);
                break;
            case CONST:
                flags |= CONST_Q;
                break;
            case VOLATILE:
                flags |= VOLATILE_Q;
                break;
            default:
                end = 1;
        }
        if (end) break;
        consume(peek());
    }
    if (type == NULL) {
        error("Missing type specifier, aborting");
        exit(1);
    }
    type->flags = flags;
    return type;
}

static typetree_t *
declarator(typetree_t *base, const char **symbol)
{
    while (peek() == '*') {
        base = pointer(base);
    }
    return direct_declarator(base, symbol);
}

static typetree_t *
pointer(const typetree_t *base)
{
    typetree_t *type = type_init(POINTER);
    type->next = base;
    base = type;
    consume('*');
    while (peek() == CONST || peek() == VOLATILE) {
        type->flags |= (readtoken().type == CONST) ? CONST_Q : VOLATILE_Q;
    }
    return type;
}

/* Consume [s0][s1][s2]..[sn] in array declarations, returning type
 * <symbol> :: [s0] [s1] [s2] .. [sn] (base)
 */
static typetree_t *
direct_declarator_array(typetree_t *base)
{
    if (peek() == '[') {
        typetree_t *root;
        var_t expr;
        long length;

        consume('[');
        if (peek() != ']') {
            block_t *throwaway = block_init();
            expr = constant_expression(throwaway);
            if (expr.kind != IMMEDIATE || expr.type->type != INT64_T) {
                error("Array declaration must be a compile time constant, aborting");
                exit(1);
            }
            length = expr.value.v_long;
            if (length < 1) {
                error("Invalid array size %ld, aborting");
                exit(1);
            }
        } else {
            /* special value for unspecified array size */
            length = 0;
        }
        consume(']');
        
        base = direct_declarator_array(base);
        root = type_init(ARRAY);

        root->next = base;
        root->length = length;
        root->size = (base->type == ARRAY) ? base->size * base->length : base->size;
        base = root;
    }
    return base;
}

static typetree_t *
direct_declarator(typetree_t *base, const char **symbol)
{
    typetree_t *type = base;
    switch (peek()) {
        case IDENTIFIER:
            /* Allocate dumplicate value, the tokenized one is temporary. */
            *symbol = strdup(readtoken().value);
            break;
        case '(':
            consume('(');
            type = declarator(base, symbol);
            consume(')');
            break;
        default: break;
    }
    /* left-recursive declarations like 'int foo[10][5];' */
    while (peek() == '[' || peek() == '(') {
        switch (peek()) {
            case '[':
                type = direct_declarator_array(base);
                break;
            case '(': {
                consume('(');
                type = parameter_list(base);
                consume(')');
                break;
            }
            default: break; /* impossible */
        }
        base = type;
    }
    return type;
}

/* FOLLOW(parameter-list) = { ')' }, peek to return empty list;
 * even though K&R require at least specifier: (void)
 * Set parameter-type-list = parameter-list, including the , ...
 */
static typetree_t *
parameter_list(const typetree_t *base)
{
    typetree_t *type = type_init(FUNCTION);
    const typetree_t **args = NULL;
    const char **params = NULL;
    int nargs = 0;

    while (peek() != ')') {
        const char *symbol = NULL;
        typetree_t *decl = declaration_specifiers();
        decl = declarator(decl, &symbol);

        nargs++;
        args = realloc(args, sizeof(typetree_t *) * nargs);
        params = realloc(params, sizeof(char *) * nargs);
        args[nargs - 1] = decl;
        params[nargs - 1] = symbol;

        if (peek() != ',') break;
        consume(',');
        if (peek() == ')') {
            error("Trailing comma in parameter list, aborting");
            exit(1);
        }
        if (peek() == DOTS) {
            consume(DOTS); /* todo: add vararg type */
            break;
        }
    }
    
    type->next = base;
    type->n_args = nargs;
    type->args = args;
    type->params = params;
    return type;
}

/* Treat statements and declarations equally, allowing declarations in between
 * statements as in modern C. Called compound-statement in K&R.
 */
static block_t *
block(block_t *parent)
{
    consume('{');
    push_scope();
    while (peek() != '}') {
        parent = statement(parent);
    }
    consume('}');
    pop_scope();
    return parent;
}

/* Create or expand a block of code. Consecutive statements without branches
 * are stored as a single block, passed as parent. Statements with branches
 * generate new blocks. Returns the current block of execution after the
 * statement is done. For ex: after an if statement, the empty fallback is
 * returned. Caller must keep handles to roots, only the tail is returned. */
static block_t *
statement(block_t *parent)
{
    block_t *node;

    /* Store reference to top of loop, for resolving break and continue. Use
     * call stack to keep track of depth, backtracking to the old value. */
    static block_t *break_target, *continue_target;
    block_t *old_break_target, *old_continue_target;

    enum token t = peek();

    switch (t) {
        case ';':
            consume(';');
            node = parent;
            break;
        case '{':
            node = block(parent); /* execution continues  */
            break;
        case SWITCH:
        case IF:
        {
            block_t *right = block_init(), *next = block_init();
            readtoken();
            consume('(');

            /* node becomes a branch, store the expression as condition
             * variable and append code to compute the value. */
            parent->expr = expression(parent);
            consume(')');

            parent->jump[0] = next;
            parent->jump[1] = right;

            /* The order is important here: Send right as head in new statement
             * graph, and store the resulting tail as new right, hooking it up
             * to the fallback of the if statement. */
            right = statement(right);
            right->jump[0] = next;

            if (peek() == ELSE) {
                block_t *left = block_init();
                consume(ELSE);

                /* Again, order is important: Set left as new jump target for
                 * false if branch, then invoke statement to get the
                 * (potentially different) tail. */
                parent->jump[0] = left;
                left = statement(left);

                left->jump[0] = next;
            }
            node = next;
            break;
        }
        case WHILE:
        case DO:
        {
            block_t *top = block_init(), *body = block_init(), *next = block_init();
            parent->jump[0] = top; /* Parent becomes unconditional jump. */

            /* Enter a new loop, store reference for break and continue target. */
            old_break_target = break_target;
            old_continue_target = continue_target;
            break_target = next;
            continue_target = top;

            readtoken();

            if (t == WHILE) {
                consume('(');
                top->expr = expression(top);
                consume(')');
                top->jump[0] = next;
                top->jump[1] = body;

                /* Generate statement, and get tail end of body to loop back */
                body = statement(body);
                body->jump[0] = top;
            } else if (t == DO) {

                /* Generate statement, and get tail end of body */
                body = statement(top);
                consume(WHILE);
                consume('(');
                body->expr = expression(body); /* Tail becomes branch. (nb: wrong if tail is return?!) */
                body->jump[0] = next;
                body->jump[1] = top;
                consume(')');
            }

            /* Restore previous nested loop */
            break_target = old_break_target;
            continue_target = old_continue_target;

            node = next;
            break;
        }
        case FOR:
        {
            block_t *top = block_init(), *body = block_init(), *increment = block_init(), *next = block_init();

            /* Enter a new loop, store reference for break and continue target. */
            old_break_target = break_target;
            old_continue_target = continue_target;
            break_target = next;
            continue_target = increment;

            consume(FOR);
            consume('(');
            if (peek() != ';') {
                expression(parent);
            }
            consume(';');
            if (peek() != ';') {
                parent->jump[0] = top;
                top->expr = expression(top);
                top->jump[0] = next;
                top->jump[1] = body;
            } else {
                /* Infinite loop */
                parent->jump[0] = body;
                top = body;
            }
            consume(';');
            if (peek() != ')') {
                expression(increment);
                increment->jump[0] = top;
            }
            consume(')');
            body = statement(body);
            body->jump[0] = increment;

            /* Restore previous nested loop */
            break_target = old_break_target;
            continue_target = old_continue_target;

            node = next;
            break;
        }
        case GOTO:
            consume(GOTO);
            identifier();
            /* todo */
            consume(';');
            break;
        case CONTINUE:
        case BREAK:
            readtoken();
            parent->jump[0] = (t == CONTINUE) ? 
                continue_target :
                break_target;
            consume(';');
            /* Return orphan node, which is dead code unless there is a label
             * and a goto statement. */
            node = block_init(); 
            break;
        case RETURN:
            consume(RETURN);
            if (peek() != ';')
                parent->expr = expression(parent);
            consume(';');
            node = block_init(); /* orphan */
            break;
        case CASE:
        case DEFAULT:
            /* todo */
            break;
        case IDENTIFIER: /* also part of label statement, need 2 lookahead */
        case INTEGER: /* todo: any constant value */
        case STRING:
        case '*':
        case '(':
            expression(parent);
            consume(';');
            node = parent;
            break;
        default: {
            const symbol_t *decl;
            node = declaration(parent, &decl);
            break;
        }
    }
    return node;
}

static const symbol_t *
identifier()
{
    token_t name = readtoken();
    const symbol_t *sym = sym_lookup(name.value);
    if (sym == NULL) {
        error("Undefined symbol '%s', aborting", name.value);
        exit(0);
    }
    return sym;
}

static var_t conditional_expression(block_t *block);
static var_t logical_expression(block_t *block);
static var_t or_expression(block_t *block);
static var_t and_expression(block_t *block);
static var_t equality_expression(block_t *block);
static var_t relational_expression(block_t *block);
static var_t shift_expression(block_t *block);
static var_t additive_expression(block_t *block);
static var_t multiplicative_expression(block_t *block);
static var_t cast_expression(block_t *block);
static var_t postfix_expression(block_t *block);
static var_t unary_expression(block_t *block);
static var_t primary_expression(block_t *block);

static var_t 
expression(block_t *block)
{
    return assignment_expression(block);
}

static var_t 
assignment_expression(block_t *block)
{
    var_t l = conditional_expression(block), r;
    if (peek() == '=') {
        consume('=');
        /* todo: node must be unary-expression or lower (l-value) */
        r = assignment_expression(block);
        l = eval_assign(block, l, r);
    }
    return l;
}

static var_t 
constant_expression(block_t *block)
{
    return conditional_expression(block);
}

static var_t 
conditional_expression(block_t *block)
{
    var_t v = logical_expression(block);
    if (peek() == '?') {
        consume('?');
        expression(block);
        consume(':');
        conditional_expression(block);
    }
    return v;
}

/* merge AND/OR */
static var_t 
logical_expression(block_t *block)
{
    var_t l, r;
    l = or_expression(block);
    while (peek() == LOGICAL_OR || peek() == LOGICAL_AND) {
        optype_t optype = (readtoken().type == LOGICAL_AND) 
            ? IR_OP_LOGICAL_AND : IR_OP_LOGICAL_OR;

        r = and_expression(block);
        l = eval_expr(block, optype, l, r);
    }
    return l;
}

/* merge OR/XOR */
static var_t
or_expression(block_t *block)
{
    var_t l, r;
    l = and_expression(block);
    while (peek() == '|' || peek() == '^') {
        optype_t optype = (readtoken().type == '|') 
            ? IR_OP_BITWISE_OR : IR_OP_BITWISE_XOR;

        r = and_expression(block);
        l = eval_expr(block, optype, l, r);
    }
    return l;
}

static var_t
and_expression(block_t *block)
{
    var_t l, r;
    l = equality_expression(block);
    while (peek() == '&') {
        consume('&');
        r = and_expression(block);
        l = eval_expr(block, IR_OP_BITWISE_AND, l, r);
    }
    return l;
}

static var_t
equality_expression(block_t *block)
{
    return relational_expression(block);
}

static var_t
relational_expression(block_t *block)
{
    return shift_expression(block);
}

static var_t
shift_expression(block_t *block)
{
    return additive_expression(block);
}

static var_t
additive_expression(block_t *block)
{
    var_t l, r;
    l = multiplicative_expression(block);
    while (peek() == '+' || peek() == '-') {
        optype_t optype = (readtoken().type == '+') ? IR_OP_ADD : IR_OP_SUB;

        r = multiplicative_expression(block);
        l = eval_expr(block, optype, l, r);
    }
    return l;
}

static var_t
multiplicative_expression(block_t *block)
{
    var_t l, r;
    l = cast_expression(block);
    while (peek() == '*' || peek() == '/' || peek() == '%') {
        token_t tok = readtoken();
        optype_t optype = (tok.type == '*') ?
            IR_OP_MUL : (tok.type == '/') ?
                IR_OP_DIV : IR_OP_MOD;

        r = cast_expression(block);
        l = eval_expr(block, optype, l, r);
    }
    return l;
}

static var_t
cast_expression(block_t *block)
{
    return unary_expression(block);
}

static var_t
unary_expression(block_t *block)
{
    var_t expr;

    switch (peek()) {
        case '&':
            consume('&');
            expr = cast_expression(block);
            expr = eval_addr(block, expr);
            break;
        case '*':
            consume('*');
            expr = cast_expression(block);
            expr = eval_deref(block, expr);
            break;
        default:
            expr = postfix_expression(block);
    }
    return expr;
}

/* This rule is left recursive, build tree bottom up
 */
static var_t
postfix_expression(block_t *block)
{
    var_t root;
    var_t expr;
    var_t addr;

    root = primary_expression(block);

    while (peek() == '[' || peek() == '(' || peek() == '.') {
        switch (peek()) {
            /* Parse and emit ir for general array indexing
             *  - From K&R: an array is not a variable, and cannot be assigned or modified.
             *    Referencing an array always converts the first rank to pointer type,
             *    e.g. int foo[3][2][1]; a = foo; assignment has the type int (*)[2][1].
             *  - Functions return and pass pointers to array. First index not necessary to
             *    specify in array (pointer) parameters: int (*foo(int arg[][3][2][1]))[3][2][1]
             */
            case '[':
                /* Evaluate a[b] = *(a + b). */
                while (peek() == '[') {
                    consume('[');
                    expr = expression(block);
                    addr = eval_expr(block, IR_OP_MUL, expr, var_long(root.type->size));
                    addr = eval_expr(block, IR_OP_ADD, root, addr);
                    root = eval_deref(block, addr);
                    consume(']');
                }
                break;
            default:
                error("Unexpected token '%s', not a valid postfix expression", readtoken().value);
                exit(0);
        }
    }
    return root;
}

static var_t
primary_expression(block_t *block)
{
    var_t var;
    token_t token;
    const symbol_t *symbol;

    token = readtoken();
    switch (token.type) {
        case IDENTIFIER:
            symbol = sym_lookup(token.value);
            if (symbol == NULL) {
                error("Undefined symbol '%s', aborting", token.value);
                exit(0);
            }
            var = var_direct(symbol);
            break;
        case INTEGER:
            var = var_long(strtol(token.value, NULL, 0));
            break;
        case '(':
            var = expression(block);
            consume(')');
            break;
        case STRING:
            var = var_string(token.value);
            break;
        default:
            error("Unexpected token '%s', not a valid primary expression", token.value);
            exit(0);
    }
    return var;
}
