#include "libgccjit.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __GNUC__
#include <execinfo.h>
static void printStack(void) {
  void *buf[10];
  int frameNum = backtrace(buf, sizeof(buf) / sizeof(void *));
  char **symbols = backtrace_symbols(buf, frameNum);

  printf("stacktrace:\n");
  for (int i = 0; i < frameNum; ++i) {
    printf("\t%s\n", symbols[i]);
  }

  free(symbols);
}
#else
static void printStack(void) { printf("(unknown)\n"); }
#endif

static void panic(const char *msg) {
  printf("panic: %s\n\n", msg);
  printStack();
  exit(1);
}

static void unreachable(void) { panic("unreachable"); }

static void *allocate(size_t size) {
  void *ret = malloc(size);
  if (ret) {
    return ret;
  }
  panic("out of memory");
  return NULL;
}

static void *allocateZeroed(size_t size, size_t count) {
  void *ret = calloc(count, size);
  if (ret) {
    return ret;
  }
  panic("out of memory");
  return NULL;
}

static void *reallocate(void *p, size_t size) {
  void *ret = realloc(p, size);
  if (ret) {
    return ret;
  }
  panic("out of memory");
  return NULL;
}

#define new(type) allocate(sizeof(type))
#define make(type, count) allocateZeroed(sizeof(type), count)

static volatile int nextUID = 0;

static int newUID(void) {
  nextUID++;
  return nextUID;
}

static int max(int a, int b) { return a > b ? a : b; }

struct node {
  struct node *Left, *Right;
  int Key, Height;
};

static void node_Default(struct node *node) {
  node->Left = NULL;
  node->Right = NULL;
  node->Height = 1;
}

static int node_height(struct node *node) { return !node ? 0 : node->Height; }

static struct node *node_rightRotate(struct node *x) {
  struct node *y = x->Left;
  struct node *z = y->Right;
  y->Right = x;
  x->Left = z;
  x->Height = max(node_height(x->Left), node_height(x->Right)) + 1;
  y->Height = max(node_height(y->Left), node_height(y->Right)) + 1;
  return y;
}

static struct node *node_leftRotate(struct node *x) {
  struct node *y = x->Right;
  struct node *z = y->Left;
  y->Left = x;
  x->Right = z;
  x->Height = max(node_height(x->Left), node_height(x->Right)) + 1;
  y->Height = max(node_height(y->Left), node_height(y->Right)) + 1;
  return y;
}

static int node_balance(struct node *node) {
  return !node ? 0 : node_height(node->Left) - node_height(node->Right);
}

static struct node *tree_Insert(struct node *root, struct node *other) {
  if (!root) {
    return other;
  }

  if (other->Key < root->Key) {
    root->Left = tree_Insert(root->Left, other);
  } else if (other->Key > root->Key) {
    root->Right = tree_Insert(root->Right, other);
  } else {
    return root;
  }

  root->Height = max(node_height(root->Left), node_height(root->Right)) + 1;

  int balance = node_balance(root);
  if (balance > 1 && other->Key < root->Left->Key) {
    return node_rightRotate(root);
  }
  if (balance < -1 && other->Key > root->Right->Key) {
    return node_leftRotate(root);
  }
  if (balance > 1 && other->Key > root->Left->Key) {
    root->Left = node_leftRotate(root->Left);
    return node_rightRotate(root);
  }
  if (balance < -1 && other->Key < root->Right->Key) {
    root->Right = node_rightRotate(root->Right);
    return node_leftRotate(root);
  }

  return root;
}

/*static*/ void tree_Iter(void *data, struct node *root,
                          void (*f)(void *data, struct node *node)) {
  if (!root) {
    return;
  }
  tree_Iter(data, root->Left, f);
  f(data, root);
  tree_Iter(data, root->Right, f);
}

/*static*/ void tree_Free(struct node *root,
                          void *(*downcast)(struct node *node)) {
  if (!root) {
    return;
  }
  tree_Free(root->Left, downcast);
  tree_Free(root->Right, downcast);
  free(downcast(root));
}

struct slice {
  uint8_t *Data;
  size_t ElemSize, Size, Cap;
};

static void slice_Init(struct slice *s, size_t elemSize) {
  s->ElemSize = elemSize;
  s->Size = 0;
  s->Cap = 4;
  s->Data = allocateZeroed(elemSize, s->Cap);
}

static void slice_Append(struct slice *s, void *value) {
  if (s->Size == s->Cap) {
    s->Cap *= 2;
    s->Data = reallocate(s->Data, s->Cap * s->ElemSize);
  }
  memcpy(s->Data + s->Size * s->ElemSize, value, s->ElemSize);
  s->Size++;
}

static void slice_Iter(void *data, struct slice *s,
                       void (*f)(void *data, void *elem)) {
  uint8_t *elem = s->Data;
  for (size_t i = 0; i < s->Size; i++, elem += s->ElemSize) {
    f(data, elem);
  }
}

/*static*/ void slice_Free(struct slice *s) {
  free(s->Data);
  s->Data = NULL;
  s->Size = 0;
  s->Cap = 0;
}

struct entry {
  struct entry *Next;
  const char *Key;
  int Val;
};

struct map {
  struct entry **Buckets;
  size_t Size, Cap;
};

static void map_Default(struct map *m) {
  m->Buckets = make(struct entry *, m->Cap);
  m->Size = 0;
  m->Cap = 8;
}

static size_t map_hash(const char *key, size_t cap) {
  size_t h = 0;
  for (char c = *key; c != '\0'; c++) {
    h = h * 31 + (size_t)c;
  }
  return h % cap;
}

static void map_rehash(struct map *m) {
  size_t newCap = m->Cap * 2;
  struct entry **newBuckets = make(struct entry *, newCap);

  for (size_t i = 0; i < m->Cap; i++) {
    struct entry *e = m->Buckets[i];
    while (e) {
      struct entry *next = e->Next;
      size_t index = map_hash(e->Key, newCap);
      e->Next = newBuckets[index];
      newBuckets[index] = e;
      e = next;
    }
  }

  free(m->Buckets);
  m->Buckets = newBuckets;
  m->Cap = newCap;
}

static bool map_Set(struct map *m, const char *key, int val) {
  if ((double)m->Size / (double)m->Cap >= 1.0) {
    map_rehash(m);
  }
  size_t index = map_hash(key, m->Cap);
  struct entry *e = m->Buckets[index];
  while (e) {
    if (strcmp(e->Key, key) == 0) {
      e->Val = val;
      return true;
    }
    e = e->Next;
  }
  e = new (struct entry);
  e->Key = key;
  e->Val = val;
  e->Next = m->Buckets[index];
  m->Buckets[index] = e;
  m->Size++;
  return false;
}

static bool map_Get(struct map *m, const char *key, int *val) {
  size_t index = map_hash(key, m->Cap);
  struct entry *e = m->Buckets[index];
  while (e) {
    if (strcmp(e->Key, key) == 0) {
      *val = e->Val;
      return true;
    }
    e = e->Next;
  }
  return false;
}

static void map_Free(struct map *m) {
  for (size_t i = 0; i < m->Cap; i++) {
    struct entry *e = m->Buckets[i];
    while (e) {
      struct entry *next = e->Next;
      if (e->Key) {
        free((void *)e->Key);
      }
      free(e);
      e = next;
    }
  }
  free(m->Buckets);
  m->Buckets = NULL;
}

static void map_Merge(struct map *lhs, struct map *rhs) {
  for (size_t i = 0; i < rhs->Cap; i++) {
    struct entry *rhs_e = rhs->Buckets[i];
    while (rhs_e) {
      struct entry *rhs_next = rhs_e->Next;
      if (map_Set(lhs, rhs_e->Key, rhs_e->Val)) {
        free((void *)rhs_e->Key);
        rhs_e->Key = NULL;
      }
      rhs_e = rhs_next;
    }
  }
  map_Free(rhs);
}

enum objectKind { object_Num = 1 };

struct object {
  enum objectKind Kind;
  bool Marked;
  struct object *Next;
};

static void object_Init(struct object *o, enum objectKind kind,
                        struct object *next) {
  o->Kind = kind;
  o->Marked = 0;
  o->Next = next;
}

static void object_Mark(struct object *o) {
  if (o->Marked) {
    return;
  }
  o->Marked = 1;
  // TODO: Mark other objects from the members.
}

struct gc {
  struct object *Stack[256];
  size_t StackSize, Reachable, Max;
  struct object *Root;
};

/*static*/ void gc_Default(struct gc *c) {
  c->StackSize = 0;
  c->Reachable = 0;
  c->Max = 8;
  c->Root = NULL;
}

/*static*/ void gc_Push(struct gc *gc, struct object *value) {
  if (gc->StackSize >= sizeof(gc->Stack) / sizeof(struct object *)) {
    printf("Stack overflow\n");
    exit(1);
  }
  gc->Stack[gc->StackSize] = value;
  gc->StackSize++;
}

/*static*/ struct object *gc_Pop(struct gc *gc) {
  if (gc->StackSize <= 0) {
    printf("Stack overflow\n");
    exit(1);
  }
  struct object *ret = gc->Stack[gc->StackSize];
  gc->StackSize--;
  return ret;
}

static void gc_Mark(struct gc *vm) {
  for (size_t i = 0; i < vm->StackSize; i++) {
    object_Mark(vm->Stack[i]);
  }
}

static void gc_Sweep(struct gc *vm) {
  struct object **object = &vm->Root;
  while (*object) {
    if ((*object)->Marked) {
      (*object)->Marked = 0;
      object = &(*object)->Next;
      continue;
    }
    struct object *unreached = *object;
    *object = unreached->Next;
    free(unreached);
    vm->Reachable--;
  }
}

static void gc_Run(struct gc *vm) {
  gc_Mark(vm);
  gc_Sweep(vm);
  vm->Max = vm->Reachable == 0 ? 8 : vm->Reachable * 2;
}

/*static*/ struct object *gc_NewObject(struct gc *vm, enum objectKind kind) {
  if (vm->Reachable == vm->Max) {
    gc_Run(vm);
  }
  struct object *o = new (struct object);
  object_Init(o, kind, vm->Root);
  vm->Root = o;
  vm->Reachable++;
  return o;
}

/*static*/ void gc_Free(struct gc *vm) {
  vm->StackSize = 0;
  gc_Run(vm);
}

struct loc {
  size_t Pos, Ln, Col;
};

static void loc_Default(struct loc *l) {
  l->Pos = 0;
  l->Ln = 1;
  l->Col = 1;
}

static void loc_NextLine(struct loc *l) {
  l->Pos++;
  l->Ln++;
  l->Col = 1;
}

static void loc_NextColumn(struct loc *l) {
  l->Pos++;
  l->Col++;
}

struct span {
  struct loc Start, End;
};

struct Source {
  struct loc Loc;
  FILE *File;
  bool Failed, Atom, NewlineSensitive;
};

static void source_Init(struct Source *s, FILE *f) {
  loc_Default(&s->Loc);
  s->File = f;
  s->Failed = false;
  s->Atom = false;
  s->NewlineSensitive = false;
}

static size_t source_Size(struct Source *s) {
  fseek(s->File, 0, SEEK_END);
  long size = ftell(s->File);
  fseek(s->File, (long)s->Loc.Pos, SEEK_SET);
  return (size_t)size;
}

static const char *source_Text(struct Source *s, struct span span) {
  size_t size = span.End.Pos - span.Start.Pos + 1;
  char *text = allocate(size);
  fseek(s->File, (long)span.Start.Pos, SEEK_SET);
  return fgets(text, (int)size, s->File);
}

static char source_Peek(struct Source *s) {
  if (fseek(s->File, (long)s->Loc.Pos, SEEK_SET) != 0) {
    return -1;
  }
  int c = fgetc(s->File);
  if (c == EOF) {
    return -1;
  }
  return (char)c;
}

static char source_Next(struct Source *s) {
  char next = source_Peek(s);
  if (next < 0) {
    return -1;
  }
  if (next == '\n') {
    loc_NextLine(&s->Loc);
    return next;
  }
  loc_NextColumn(&s->Loc);
  return next;
}

static struct Source *source_Back(struct Source *s, struct loc loc) {
  s->Loc = loc;
  s->Failed = false;
  return s;
}

static struct Source *source_Eat(struct Source *s, char c) {
  if (source_Next(s) != c) {
    s->Failed = true;
  }
  return s;
}

static struct Source *source_SkipSpaces(struct Source *s) {
  while (true) {
    char c = source_Peek(s);
    if (c < 0 || (s->NewlineSensitive && c == '\n') || !isspace(c)) {
      break;
    }
    s = source_Eat(s, c);
  }
  return s;
}

struct Parser;
struct Expr;
struct Def;

struct Range {
  char From, To;
};

union ParserCtx {
  const char *Word;
  struct Range Range;
  struct Parser *Parser;
  struct Parser **Parsers;

  struct span *Span;
  struct slice *Slice;
  struct node **Nodes;
  struct Expr *Expr;
  struct Def *Def;
};

struct Parser {
  struct Source *(*Parse)(union ParserCtx *ctx, struct Source *s);
  union ParserCtx Ctx;
};

static struct Source *soi(union ParserCtx *ctx, struct Source *s) {
  (void)ctx;
  if (s->Loc.Pos != 0) {
    s->Failed = true;
  }
  return s;
}

static struct Source *eoi(union ParserCtx *ctx, struct Source *s) {
  (void)ctx;
  if (s->Loc.Pos != source_Size(s)) {
    s->Failed = true;
  }
  return s;
}

static struct Parser Soi = {.Parse = soi};
static struct Parser Eoi = {.Parse = eoi};

static struct Source *parseAtom(struct Parser *parser, struct Source *s) {
  int atom = s->Atom;
  s->Atom = true;
  s = parser->Parse(&parser->Ctx, s);
  s->Atom = atom;
  return s;
}

static struct Source *word(union ParserCtx *ctx, struct Source *s) {
  const char *word = ctx->Word;
  size_t i = 0;
  for (char c = word[i]; c != '\0'; i++, c = word[i]) {
    s = source_Eat(s, c);
    if (s->Failed) {
      break;
    }
  }
  return s;
}

static struct Parser LParen = {word, {.Word = "("}};
static struct Parser RParen = {word, {.Word = ")"}};
static struct Parser Comma = {word, {.Word = ","}};
static struct Parser Under = {word, {.Word = "_"}};
static struct Parser Newline = {word, {.Word = "\n"}};
static struct Parser Semicolon = {word, {.Word = ";"}};
static struct Parser Assign = {word, {.Word = "="}};
static struct Parser If = {word, {.Word = "if"}};
static struct Parser Then = {word, {.Word = "then"}};
static struct Parser Else = {word, {.Word = "else"}};
static struct Parser Arrow = {word, {.Word = "=>"}};
static struct Parser Unit = {word, {.Word = "()"}};
static struct Parser False = {word, {.Word = "false"}};
static struct Parser True = {word, {.Word = "true"}};

static struct Source *range(union ParserCtx *ctx, struct Source *s) {
  char from = ctx->Range.From, to = ctx->Range.To;
  char c = source_Peek(s);
  if (c < from || c > to) {
    s->Failed = true;
    return s;
  }
  return source_Eat(s, c);
}

// static struct Parser AsciiBinDigit = {Range, {.Range = {'0', '1'}}};
// static struct Parser AsciiOctDigit = {Range, {.Range = {'0', '7'}}};
static struct Parser AsciiDigit = {range, {.Range = {'0', '9'}}};
// static struct Parser AsciiNonZeroDigit = {Range, {.Range = {'1', '9'}}};

static struct Source *parseLowercase(struct span *span, struct Source *s) {
  struct loc start = s->Loc;

  char first = source_Peek(s);
  if (first < 0 || !islower(first) || !isalpha(first)) {
    s->Failed = true;
    return s;
  }
  s = source_Eat(s, first);

  while (true) {
    char c = source_Peek(s);
    if (!(islower(c) && isalpha(c)) && c != '_') {
      break;
    }
    s = source_Eat(s, c);
  }

  *span = (struct span){start, s->Loc};
  return s;
}

static struct Source *lowercase(union ParserCtx *ctx, struct Source *s) {
  return parseLowercase(ctx->Span, s);
}

static struct Source *parseAll(struct Parser **parsers, struct Source *s) {
  struct Parser **parser = parsers;
  while (*parser) {
    s = (*parser)->Parse(&(*parser)->Ctx, s);
    if (s->Failed) {
      return s;
    }
    parser++;
    if (!s->Atom && *parser) {
      s = source_SkipSpaces(s);
    }
  }
  return s;
}

static struct Source *all(union ParserCtx *ctx, struct Source *s) {
  return parseAll(ctx->Parsers, s);
}

static struct Source *parseAny(struct Parser **parsers, struct Source *s) {
  struct loc loc = s->Loc;
  for (struct Parser **parser = parsers; *parser; parser++) {
    s = (*parser)->Parse(&(*parser)->Ctx, s);
    if (!s->Failed) {
      return s;
    }
    s = source_Back(s, loc);
  }
  s->Failed = true;
  return s;
}

static struct Source *any(union ParserCtx *ctx, struct Source *s) {
  return parseAny(ctx->Parsers, s);
}

static struct Parser *EndSymbols[] = {&Semicolon, &Newline, NULL};
static struct Parser End = {any, {.Parsers = EndSymbols}};

static struct Source *parseEnd(struct Source *s) {
  int sensitive = s->NewlineSensitive;
  s->NewlineSensitive = true;
  s = source_SkipSpaces(s);
  s = End.Parse(&End.Ctx, s);
  s->NewlineSensitive = sensitive;
  return s;
}

static struct Source *many(union ParserCtx *ctx, struct Source *s) {
  while (true) {
    struct loc loc = s->Loc;
    s = ctx->Parser->Parse(&ctx->Parser->Ctx, s);
    if (s->Failed) {
      return source_Back(s, loc);
    }
    if (!s->Atom) {
      s = source_SkipSpaces(s);
    }
  }
}

static struct Source *option(union ParserCtx *ctx, struct Source *s) {
  struct loc loc = s->Loc;
  s = ctx->Parser->Parse(&ctx->Parser->Ctx, s);
  if (s->Failed) {
    return source_Back(s, loc);
  }
  return s;
}

enum ExprKind {
  Expr_App = 1,
  Expr_Ite,
  Expr_Lam,
  Expr_Num,
  Expr_Unit,
  Expr_False,
  Expr_True,
  Expr_Unresolved,
  Expr_Resolved,
};
union ExprData {
  struct App *App;
  struct Ite *Ite;
  struct Lambda *Lam;
  struct span Span;
  int ID;
};
struct Expr {
  enum ExprKind kind;
  union ExprData data;
};

struct Source *ParseExpr(struct Expr *expr, struct Source *s);

struct Source *Expr(union ParserCtx *ctx, struct Source *s) {
  return ParseExpr(ctx->Expr, s);
}

struct App {
  struct Expr F;
  struct slice Args;
};

void App_Default(struct App *a) { slice_Init(&a->Args, sizeof(struct Expr)); }

struct Source *Arg(union ParserCtx *ctx, struct Source *s) {
  struct Expr a;
  s = ParseExpr(&a, s);
  if (!s->Failed) {
    slice_Append(ctx->Slice, &a);
  }
  return s;
}

struct Source *Args(union ParserCtx *ctx, struct Source *s) {
  struct Parser *no_args[] = {&LParen, &RParen, NULL};
  struct Parser all_no_args = {all, {.Parsers = no_args}};

  struct Parser one_arg = {Arg, {.Slice = ctx->Slice}};
  struct Parser *other_args[] = {&Comma, &one_arg, NULL};
  struct Parser all_other_args = {all, {.Parsers = other_args}};
  struct Parser many_other_args = {many, {.Parser = &all_other_args}};
  struct Parser *multi_args[] = {&LParen, &one_arg, &many_other_args, &RParen,
                                 NULL};
  struct Parser all_multi_args = {all, {.Parsers = multi_args}};

  struct Parser *branches[] = {&all_no_args, &all_multi_args, NULL};
  return parseAny(branches, s);
}

static struct Source *Expr_ref(union ParserCtx *ctx, struct Source *s);
static struct Source *Expr_paren(union ParserCtx *ctx, struct Source *s);

static struct Source *Expr_app(union ParserCtx *ctx, struct Source *s) {
  struct App *app = new (struct App);
  App_Default(app);

  struct Parser fnRef = {Expr_ref, {.Expr = &app->F}};
  struct Parser fnExpr = {Expr_paren, {.Expr = &app->F}};
  struct Parser *fnParsers[] = {&fnRef, &fnExpr, NULL};
  struct Parser f = {any, {.Parsers = fnParsers}};

  struct Parser xs = {Args, {.Slice = &app->Args}};

  struct Parser *parsers[] = {&f, &xs, NULL};
  s = parseAll(parsers, s);
  if (s->Failed) {
    free(app);
    return s;
  }
  ctx->Expr->data.App = app;
  ctx->Expr->kind = Expr_App;
  return s;
}

struct Ite {
  struct Expr i, t, e;
};

static struct Source *Expr_ite(union ParserCtx *ctx, struct Source *s) {
  struct Ite *ite = new (struct Ite);
  struct Parser i = {Expr, {.Expr = &ite->i}};
  struct Parser t = {Expr, {.Expr = &ite->t}};
  struct Parser e = {Expr, {.Expr = &ite->e}};
  struct Parser *parsers[] = {&If, &i, &Then, &t, &Else, &e, NULL};
  s = parseAll(parsers, s);
  if (s->Failed) {
    free(ite);
    return s;
  }
  ctx->Expr->kind = Expr_Ite;
  ctx->Expr->data.Ite = ite;
  return s;
}

struct Param {
  struct node AsNode;
  struct span Name;
};

struct Source *Param(union ParserCtx *ctx, struct Source *s) {
  struct span name;
  s = parseLowercase(&name, s);
  if (s->Failed) {
    return s;
  }
  struct Param *param = new (struct Param);
  node_Default(&param->AsNode);
  param->Name = name;
  param->AsNode.Key = newUID();
  *ctx->Nodes = tree_Insert(*ctx->Nodes, &param->AsNode);
  return s;
}

struct Source *Params(union ParserCtx *ctx, struct Source *s) {
  struct Parser *noParams[] = {&LParen, &RParen, NULL};
  struct Parser allNoParams = {all, {.Parsers = noParams}};

  struct Parser oneParam = {Param, {.Nodes = ctx->Nodes}};
  struct Parser *otherParams[] = {&Comma, &oneParam, NULL};
  struct Parser allOtherParams = {all, {.Parsers = otherParams}};
  struct Parser manyOtherParams = {many, {.Parser = &allOtherParams}};
  struct Parser *multiParams[] = {&LParen, &oneParam, &manyOtherParams, &RParen,
                                  NULL};
  struct Parser allMultiParams = {all, {.Parsers = multiParams}};

  struct Parser *branches[] = {&allNoParams, &allMultiParams, NULL};
  return parseAny(branches, s);
}

struct Lambda {
  struct node *Params;
  struct Expr Body;
};

void Lambda_Default(struct Lambda *lam) { lam->Params = NULL; }

static struct Source *Expr_lambda(union ParserCtx *ctx, struct Source *s) {
  struct Lambda *lam = new (struct Lambda);
  Lambda_Default(lam);
  struct Parser ps = {Params, {.Nodes = &lam->Params}};
  struct Parser body = {Expr, {.Expr = &lam->Body}};
  struct Parser *parsers[] = {&ps, &Arrow, &body, NULL};
  s = parseAll(parsers, s);
  if (s->Failed) {
    free(lam);
    return s;
  }
  ctx->Expr->kind = Expr_Lam;
  ctx->Expr->data.Lam = lam;
  return s;
}

static struct Source *decimalDigits(union ParserCtx *ctx, struct Source *s) {
  struct loc loc = s->Loc;
  struct Parser optionalUnder = {option, {.Parser = &Under}};
  struct Parser *otherDigits[] = {&optionalUnder, &AsciiDigit, NULL};
  struct Parser allOtherDigits = {all, {.Parsers = otherDigits}};
  struct Parser manyOtherDigits = {many, {.Parser = &allOtherDigits}};
  struct Parser *digits[] = {&AsciiDigit, &manyOtherDigits, NULL};
  s = parseAll(digits, s);
  if (!s->Failed) {
    *ctx->Span = (struct span){loc, s->Loc};
  }
  return s;
}

static struct Source *decimalNumber(union ParserCtx *ctx, struct Source *s) {
  struct Parser parser = {decimalDigits, {.Span = ctx->Span}};
  return parseAtom(&parser, s);
}

struct Source *Number(union ParserCtx *ctx, struct Source *s) {
  return decimalNumber(ctx, s);
}

static struct Source *Expr_number(union ParserCtx *ctx, struct Source *s) {
  struct span num;
  union ParserCtx num_ctx = {.Span = &num};
  s = Number(&num_ctx, s);
  if (!s->Failed) {
    ctx->Expr->kind = Expr_Num;
    ctx->Expr->data.Span = num;
  }
  return s;
}

static struct Source *Expr_unit(union ParserCtx *ctx, struct Source *s) {
  s = Unit.Parse(&Unit.Ctx, s);
  if (!s->Failed) {
    ctx->Expr->kind = Expr_Unit;
  }
  return s;
}

static struct Source *Expr_false(union ParserCtx *ctx, struct Source *s) {
  s = False.Parse(&False.Ctx, s);
  if (!s->Failed) {
    ctx->Expr->kind = Expr_False;
  }
  return s;
}

static struct Source *Expr_true(union ParserCtx *ctx, struct Source *s) {
  s = True.Parse(&True.Ctx, s);
  if (!s->Failed) {
    ctx->Expr->kind = Expr_True;
  }
  return s;
}

static struct Source *Expr_ref(union ParserCtx *ctx, struct Source *s) {
  struct span ref;
  s = parseLowercase(&ref, s);
  if (!s->Failed) {
    ctx->Expr->kind = Expr_Unresolved;
    ctx->Expr->data.Span = ref;
  }
  return s;
}

static struct Source *Expr_paren(union ParserCtx *ctx, struct Source *s) {
  struct Parser e = {Expr, {.Expr = ctx->Expr}};
  struct Parser *parsers[] = {&LParen, &e, &RParen, NULL};
  return parseAll(parsers, s);
}

struct Source *ParseExpr(struct Expr *expr, struct Source *s) {
  struct Parser app = {Expr_app, {.Expr = expr}};
  struct Parser ite = {Expr_ite, {.Expr = expr}};
  struct Parser lam = {Expr_lambda, {.Expr = expr}};
  struct Parser num = {Expr_number, {.Expr = expr}};
  struct Parser unit = {Expr_unit, {.Expr = expr}};
  struct Parser fl = {Expr_false, {.Expr = expr}};
  struct Parser tr = {Expr_true, {.Expr = expr}};
  struct Parser ref = {Expr_ref, {.Expr = expr}};
  struct Parser paren = {Expr_paren, {.Expr = expr}};
  struct Parser *branches[] = {&app, &ite, &lam, &num,   &unit,
                               &fl,  &tr,  &ref, &paren, NULL};
  return parseAny(branches, s);
}

enum BodyKind { Body_Fn = 1, Body_Val };
union Body {
  struct Expr Ret;
};

struct Def {
  struct node AsNode;

  struct span Name;
  struct node *Params;
  enum BodyKind Kind;
  union Body Body;
};

void Def_Default(struct Def *d) {
  node_Default(&d->AsNode);
  d->Params = NULL;
}

struct Source *Fn(union ParserCtx *ctx, struct Source *s) {
  struct Parser name = {lowercase, {.Span = &ctx->Def->Name}};
  struct Parser ps = {Params, {.Nodes = &ctx->Def->Params}};
  struct Parser ret = {Expr, {.Expr = &ctx->Def->Body.Ret}};
  struct Parser *parsers[] = {&name, &ps, &ret, NULL};
  s = parseAll(parsers, s);
  if (s->Failed) {
    return s;
  }
  s = parseEnd(s);
  if (!s->Failed) {
    ctx->Def->Kind = Body_Fn;
  }
  return s;
}

struct Source *Val(union ParserCtx *ctx, struct Source *s) {
  struct Parser name = {lowercase, {.Span = &ctx->Def->Name}};
  struct Parser ret = {Expr, {.Expr = &ctx->Def->Body.Ret}};
  struct Parser *parsers[] = {&name, &Assign, &ret, NULL};
  s = parseAll(parsers, s);
  if (s->Failed) {
    return s;
  }
  s = parseEnd(s);
  if (!s->Failed) {
    ctx->Def->Kind = Body_Val;
  }
  return s;
}

struct Source *Def(union ParserCtx *ctx, struct Source *s) {
  struct Def *d = new (struct Def);
  Def_Default(d);

  struct Parser fn = {Fn, {.Def = d}};
  struct Parser val = {Val, {.Def = d}};

  struct Parser *branches[] = {&fn, &val, NULL};
  s = parseAny(branches, s);
  if (s->Failed) {
    free(d);
    return s;
  }
  d->AsNode.Key = newUID();
  *ctx->Nodes = tree_Insert(*ctx->Nodes, &d->AsNode);
  return s;
}

struct Program {
  struct node *Defs;
};

void Program_Default(struct Program *p) { p->Defs = NULL; }

struct Source *ParseProgram(struct node **defs, struct Source *s) {
  struct Parser oneDef = {Def, {.Nodes = defs}};
  struct Parser manyDefs = {many, {.Parser = &oneDef}};
  struct Parser *parsers[] = {&Soi, &manyDefs, &Eoi, NULL};
  return parseAll(parsers, s);
}

enum Resolution { Resolution_OK, Resolution_NotFound, Resolution_Duplicate };

struct Resolver {
  struct Source *Src;
  struct map Globals, Locals, Params;
  enum Resolution State;
  struct span NameSpan;
  const char *NameText;
};

void Resolver_Init(struct Resolver *r, struct Source *s) {
  r->Src = s;
  map_Default(&r->Globals);
  r->State = Resolution_OK;
  r->NameText = NULL;
}

void Resolver_Free(struct Resolver *r) {
  map_Free(&r->Globals);
  if (r->NameText) {
    free((void *)r->NameText);
  }
}

void Resolver_Expr(struct Resolver *r, struct Expr *e);

static void Resolver_resolveArg(void *data, void *arg) {
  Resolver_Expr((struct Resolver *)data, (struct Expr *)arg);
}

static void Resolver_validateLocal(void *data, struct node *node) {
  struct Resolver *r = data;
  if (r->State != Resolution_OK) {
    return;
  }
  struct Param *p = (struct Param *)node;
  const char *nameText = source_Text(r->Src, p->Name);
  if (map_Set(&r->Params, nameText, p->AsNode.Key)) {
    r->State = Resolution_Duplicate;
    r->NameSpan = p->Name;
    r->NameText = nameText;
  }
}

static void Resolver_insertLocal(void *data, struct node *node) {
  struct Resolver *r = data;
  if (r->State != Resolution_OK) {
    return;
  }
  struct Param *p = (struct Param *)node;
  const char *nameText = source_Text(r->Src, p->Name);
  if (map_Set(&r->Locals, nameText, p->AsNode.Key)) {
    free((void *)nameText);
  }
}

static void Resolver_insertLocals(struct Resolver *r, struct node *params) {
  map_Default(&r->Params);
  tree_Iter(r, params, Resolver_validateLocal);
  if (r->State != Resolution_OK) {
    return;
  }
  map_Default(&r->Locals);
  map_Merge(&r->Locals, &r->Params);
  tree_Iter(r, params, Resolver_insertLocal);
}

void Resolver_Expr(struct Resolver *r, struct Expr *e) {
  switch (e->kind) {
  case Expr_App: {
    Resolver_Expr(r, &e->data.App->F);
    if (r->State != Resolution_OK) {
      return;
    }
    slice_Iter(r, &e->data.App->Args, Resolver_resolveArg);
    return;
  }
  case Expr_Ite: {
    Resolver_Expr(r, &e->data.Ite->i);
    if (r->State != Resolution_OK) {
      return;
    }
    Resolver_Expr(r, &e->data.Ite->t);
    if (r->State != Resolution_OK) {
      return;
    }
    Resolver_Expr(r, &e->data.Ite->e);
    return;
  }
  case Expr_Lam: {
    Resolver_insertLocals(r, e->data.Lam->Params);
    if (r->State != Resolution_OK) {
      return;
    }
    Resolver_Expr(r, &e->data.Lam->Body);
    return;
  }
  case Expr_Unresolved: {
    const char *nameText = source_Text(r->Src, e->data.Span);
    int id;
    if (map_Get(&r->Locals, nameText, &id) ||
        map_Get(&r->Globals, nameText, &id)) {
      free((void *)nameText);
      e->kind = Expr_Resolved;
      e->data.ID = id;
      return;
    }
    r->State = Resolution_NotFound;
    r->NameSpan = e->data.Span;
    r->NameText = nameText;
    return;
  }
  case Expr_Num:
  case Expr_Unit:
  case Expr_False:
  case Expr_True:
    return;
  case Expr_Resolved:
    break;
  }
  unreachable();
}

static void Resolver_insertGlobal(void *data, struct node *node) {
  struct Resolver *r = data;
  if (r->State != Resolution_OK) {
    return;
  }

  struct Def *d = (struct Def *)node;
  const char *name_text = source_Text(r->Src, d->Name);
  if (map_Set(&r->Globals, name_text, d->AsNode.Key)) {
    r->State = Resolution_Duplicate;
    r->NameSpan = d->Name;
    r->NameText = name_text;
    return;
  }

  Resolver_insertLocals(r, d->Params);
  if (r->State != Resolution_OK) {
    return;
  }

  Resolver_Expr(r, &d->Body.Ret);
  map_Free(&r->Locals);
}

void Resolver_Program(struct Resolver *r, struct Program *p) {
  tree_Iter(r, p->Defs, Resolver_insertGlobal);
}

struct Driver {
  const char *Filename;
  FILE *Infile;
  struct Source Src;
};

int Driver_Init(struct Driver *i, int argc, const char *argv[]) {
  if (argc < 2) {
    return -1;
  }
  i->Filename = argv[1];
  i->Infile = fopen(i->Filename, "r");
  if (!i->Infile) {
    perror("open file error");
    return -1;
  }
  source_Init(&i->Src, i->Infile);
  return 0;
}

int Driver_Free(struct Driver *i) {
  int ret = fclose(i->Infile);
  if (ret != 0) {
    perror("close file error");
  }
  return ret;
}

static void debugParam(void *data, struct node *node) {
  (void)data;
  struct Param *param = (struct Param *)node;
  printf("Param: key=%d, pos=%lu\n", param->AsNode.Key, param->Name.Start.Pos);
}

static void debugDef(void *data, struct node *node) {
  struct Def *d = (struct Def *)node;
  printf("Def: key=%d, pos=%lu, Kind=%d, ret_kind=%d\n", d->AsNode.Key,
         d->Name.Start.Pos, d->Kind, d->Body.Ret.kind);
  tree_Iter(data, d->Params, debugParam);
}

#if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer) &&   \
    !__has_feature(memory_sanitizer)
static void onSignal(int sig) { panic(strerror(sig)); }
static void recovery(void) { signal(SIGSEGV, onSignal); }
#else
static void recovery(void) {}
#endif

int main(int argc, const char *argv[]) {
  recovery();

  // Parsing some text.
  struct Driver driver;
  if (Driver_Init(&driver, argc, argv) != 0) {
    printf("usage: oxn FILE\n");
    return 1;
  }

  struct Program p;
  Program_Default(&p);
  struct Source *s = ParseProgram(&p.Defs, &driver.Src);
  if (s->Failed) {
    printf("%s:%lu:%lu: Parse error (pos=%lu)\n", driver.Filename, s->Loc.Ln,
           s->Loc.Col, s->Loc.Pos);
    return 1;
  }
  tree_Iter(NULL, p.Defs, debugDef);
  if (Driver_Free(&driver) != 0) {
    return 1;
  }

  // JIT example below.

  gcc_jit_context *ctx = gcc_jit_context_acquire();
  if (!ctx) {
    printf("acquire JIT context error\n");
    return 1;
  }

  gcc_jit_context_set_bool_option(ctx, GCC_JIT_BOOL_OPTION_DUMP_GENERATED_CODE,
                                  1);

  gcc_jit_type *void_type = gcc_jit_context_get_type(ctx, GCC_JIT_TYPE_VOID);
  gcc_jit_type *const_char_ptr_type =
      gcc_jit_context_get_type(ctx, GCC_JIT_TYPE_CONST_CHAR_PTR);
  gcc_jit_param *param_name =
      gcc_jit_context_new_param(ctx, NULL, const_char_ptr_type, "name");
  gcc_jit_function *func =
      gcc_jit_context_new_function(ctx, NULL, GCC_JIT_FUNCTION_EXPORTED,
                                   void_type, "say_hi", 1, &param_name, 0);
  gcc_jit_param *param_format =
      gcc_jit_context_new_param(ctx, NULL, const_char_ptr_type, "format");
  gcc_jit_function *printf_func = gcc_jit_context_new_function(
      ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
      gcc_jit_context_get_type(ctx, GCC_JIT_TYPE_INT), "printf", 1,
      &param_format, 1);

  gcc_jit_rvalue *args[2];
  args[0] = gcc_jit_context_new_string_literal(ctx, "Hello, %s!\n");
  args[1] = gcc_jit_param_as_rvalue(param_name);

  gcc_jit_block *block = gcc_jit_function_new_block(func, NULL);
  gcc_jit_block_add_eval(
      block, NULL, gcc_jit_context_new_call(ctx, NULL, printf_func, 2, args));
  gcc_jit_block_end_with_void_return(block, NULL);

  gcc_jit_result *ret = gcc_jit_context_compile(ctx);
  if (!ret) {
    printf("JIT compile error\n");
    return 1;
  }

  void (*say_hi)(const char *) =
      (void (*)(const char *))gcc_jit_result_get_code(ret, "say_hi");
  if (!say_hi) {
    printf("get code error\n");
    return 1;
  }

  say_hi("Oxn");

  gcc_jit_context_release(ctx);
  gcc_jit_result_release(ret);

  return 0;
}
