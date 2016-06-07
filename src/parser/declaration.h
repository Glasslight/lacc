#ifndef DECLARATION_H
#define DECLARATION_H

#include <lacc/ir.h>

struct block *declaration(struct definition *def, struct block *parent);

struct typetree *declarator(struct typetree *base, String *name);

struct typetree *declaration_specifiers(int *stc);

#endif
