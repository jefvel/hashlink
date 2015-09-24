/*
 * Copyright (C)2015 Haxe Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "hl.h"

#define OP(_,n) n,
#define OP_BEGIN static int hl_op_nargs[] = {
#define OP_END };
#include "opcodes.h"

#define OP(n,_) #n,
#define OP_BEGIN static const char *hl_op_names[] = {
#define OP_END };
#include "opcodes.h"

typedef struct {
	const unsigned char *b;
	int size;
	int pos;
	const char *error;
	hl_code *code;
} hl_reader;

#define READ() hl_read_b(r)
#define INDEX() hl_read_index(r)
#define UINDEX() hl_read_uindex(r)
#define ERROR(msg) if( !r->error ) r->error = msg;
#define CHK_ERROR() if( r->error ) return

static unsigned char hl_read_b( hl_reader *r ) {
	if( r->pos >= r->size ) {
		ERROR("No more data");
		return 0;
	}
	return r->b[r->pos++];
}

static void hl_read_bytes( hl_reader *r, void *data, int size ) {
	if( size < 0 ) {
		ERROR("Invalid size");
		return;
	}
	if( r->pos + size > r->size ) {
		ERROR("No more data");
		return;
	}
	memcpy(data,r->b + r->pos, size);
	r->pos += size;
}

static double hl_read_double( hl_reader *r ) {
	union {
		double d;
		char b[8];
	} s;
	hl_read_bytes(r, s.b, 8);
	return s.d;
}

static int hl_read_i32( hl_reader *r ) {
	unsigned char a, b, c, d;
	if( r->pos + 4 > r->size ) {
		ERROR("No more data");
		return 0;
	}
	a = r->b[r->pos++];
	b = r->b[r->pos++];
	c = r->b[r->pos++];
	d = r->b[r->pos++];
	return a | (b<<8) | (c<<16) | (d<<24);
}

static int hl_read_index( hl_reader *r ) {
	unsigned char b = READ();
	if( (b & 0x80) == 0 )
		return b & 0x7F;
	if( (b & 0x40) == 0 ) {
		int v = READ() | ((b & 31) << 8);
		return (b & 0x20) == 0 ? v : -v;
	}
	{
		int c = READ();
		int d = READ();
		int e = READ();
		int v = ((b & 31) << 24) | (c << 16) | (d << 8) | e;
		return (b & 0x20) == 0 ? v : -v;
	}
}

static int hl_read_uindex( hl_reader *r ) {
	int i = hl_read_index(r);
	if( i < 0 ) {
		ERROR("Negative index");
		return 0;
	}
	return i;
}

static hl_type *hl_get_type( hl_reader *r ) {
	int i = INDEX();
	if( i < 0 || i >= r->code->ntypes ) {
		ERROR("Invalid type index");
		i = 0;
	}
	return r->code->types + i;
}

static const char *hl_get_string( hl_reader *r ) {
	int i = INDEX();
	if( i < 0 || i >= r->code->nstrings ) {
		ERROR("Invalid string index");
		i = 0;
	}
	return r->code->strings[i];
}

static int hl_get_global( hl_reader *r ) {
	int g = INDEX();
	if( g < 0 || g >= r->code->nglobals ) {
		ERROR("Invalid global index");
		g = 0;
	}
	return g;
}

static void hl_read_type( hl_reader *r, hl_type *t ) {
	int i;
	t->kind = READ();
	if( t->kind >= HLAST ) {
		ERROR("Invalid type");
		return;
	}
	switch( t->kind ) {
	case HFUN:
		t->nargs = READ();
		t->args = (hl_type**)hl_malloc(&r->code->alloc, sizeof(hl_type*)*t->nargs);
		if( t->args == NULL ) {
			ERROR("Out of memory");
			return;
		}
		for(i=0;i<t->nargs;i++)
			t->args[i] = hl_get_type(r);
		t->ret = hl_get_type(r);
		break;
	default:
		break;
	}
}

static void hl_read_opcode( hl_reader *r, hl_function *f, hl_opcode *o ) {
	o->op = (hl_op)READ();
	if( o->op >= OLast ) {
		ERROR("Invalid opcode");
		return;
	}
	switch( hl_op_nargs[o->op] ) {
	case 0:
		break;
	case 1:
		o->p1 = INDEX();
		break;
	case 2:
		o->p1 = INDEX();
		o->p2 = INDEX();
		break;
	case 3:
		o->p1 = INDEX();
		o->p2 = INDEX();
		o->p3 = INDEX();
		break;
	case 4:
		o->p1 = INDEX();
		o->p2 = INDEX();
		o->p3 = INDEX();
		o->extra = (int*)INDEX();
		break;
	case -1:
		switch( o->op ) {
		case OCallN:
			{
				int i;
				o->p1 = INDEX();
				o->p2 = INDEX();
				o->p3 = READ();
				o->extra = (int*)hl_malloc(&r->code->alloc,sizeof(int) * o->p3);
				if( o->extra == NULL ) {
					ERROR("Out of memory");
					return;
				}
				for(i=0;i<o->p3;i++)
					o->extra[i] = INDEX();
			}
			break;
		default:
			ERROR("Don't know how to process opcode");
			break;
		}
	default:
		{
			int i, size = hl_op_nargs[o->op] - 3;
			o->p1 = INDEX();
			o->p2 = INDEX();
			o->p3 = INDEX();
			o->extra = (int*)hl_malloc(&r->code->alloc,sizeof(int) * size);
			if( o->extra == NULL ) {
				ERROR("Out of memory");
				return;
			}
			for(i=0;i<size;i++)
				o->extra[i] = INDEX();
		}
		break;
	}
}

static void hl_read_function( hl_reader *r, hl_function *f ) {
	int i;
	f->global = UINDEX();
	f->nregs = UINDEX();
	f->nops = UINDEX();
	f->regs = (hl_type**)hl_malloc(&r->code->alloc, f->nregs * sizeof(hl_type*));
	if( f->regs == NULL ) {
		ERROR("Out of memory");
		return;
	}
	for(i=0;i<f->nregs;i++)
		f->regs[i] = hl_get_type(r);
	CHK_ERROR();
	f->ops = (hl_opcode*)hl_malloc(&r->code->alloc, f->nops * sizeof(hl_opcode));
	if( f->ops == NULL ) {
		ERROR("Out of memory");
		return;
	}
	for(i=0;i<f->nops;i++)
		hl_read_opcode(r, f, f->ops+i);
}

#undef CHK_ERROR
#define CHK_ERROR() if( r->error ) { if( c ) hl_free(&c->alloc); printf("%s\n", r->error); return NULL; }
#define EXIT(msg) { ERROR(msg); CHK_ERROR(); }
#define ALLOC(v,ptr,count) { v = (ptr *)hl_zalloc(&c->alloc,count*sizeof(ptr)); if( v == NULL ) EXIT("Out of memory"); }

const char *hl_op_name( int op ) {
	if( op < 0 || op >= OLast )
		return "UnknownOp";
	return hl_op_names[op];
}

hl_code *hl_code_read( const unsigned char *data, int size ) {
	hl_reader _r = { data, size, 0, 0, NULL };	
	hl_reader *r = &_r;
	hl_code *c;
	hl_alloc alloc;
	int i;
	hl_alloc_init(&alloc);
	c = hl_zalloc(&alloc,sizeof(hl_code));
	if( c == NULL )
		EXIT("Out of memory");
	c->alloc = alloc;
	if( READ() != 'H' || READ() != 'L' || READ() != 'B' )
		EXIT("Invalid header");
	r->code = c;
	c->version = READ();
	if( c->version <= 0 || c->version > 1 ) {
		printf("VER=%d\n",c->version);
		EXIT("Unsupported version");
	}
	c->nints = UINDEX();
	c->nfloats = UINDEX();
	c->nstrings = UINDEX();
	c->ntypes = UINDEX();
	c->nglobals = UINDEX();
	c->nnatives = UINDEX();
	c->nfunctions = UINDEX();
	c->entrypoint = UINDEX();	
	CHK_ERROR();
	ALLOC(c->ints, int, c->nints);
	for(i=0;i<c->nints;i++)
		c->ints[i] = hl_read_i32(r);
	CHK_ERROR();
	ALLOC(c->floats, double, c->nfloats);
	for(i=0;i<c->nfloats;i++)
		c->floats[i] = hl_read_double(r);
	CHK_ERROR();
	{
		int size = hl_read_i32(r);
		char *sdata;
		CHK_ERROR();
		sdata = (char*)hl_malloc(&c->alloc,sizeof(char) * size);
		if( sdata == NULL )
			EXIT("Out of memory");
		hl_read_bytes(r, sdata, size);
		c->strings_data = sdata;
		ALLOC(c->strings, char*, c->nstrings);
		ALLOC(c->strings_lens, int, c->nstrings);
		for(i=0;i<c->nstrings;i++) {
			int sz = UINDEX();
			c->strings[i] = sdata;
			c->strings_lens[i] = sz;
			sdata += sz;
			if( sdata >= c->strings_data + size || *sdata )
				EXIT("Invalid string");
			sdata++;
		}
	}
	CHK_ERROR();
	ALLOC(c->types, hl_type, c->ntypes);
	for(i=0;i<c->ntypes;i++) {
		hl_read_type(r, c->types + i);
		CHK_ERROR();
	}
	ALLOC(c->globals, hl_type*, c->nglobals);
	for(i=0;i<c->nglobals;i++)
		c->globals[i] = hl_get_type(r);
	CHK_ERROR();
	ALLOC(c->natives, hl_native, c->nnatives);
	for(i=0;i<c->nnatives;i++) {
		c->natives[i].name = hl_get_string(r);
		c->natives[i].global = hl_get_global(r);
	}
	CHK_ERROR();
	ALLOC(c->functions, hl_function, c->nfunctions);
	for(i=0;i<c->nfunctions;i++) {
		hl_read_function(r,c->functions+i);
		CHK_ERROR();
	}
	CHK_ERROR();
	return c;
}