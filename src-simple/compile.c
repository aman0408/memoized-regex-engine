// Copyright 2007-2009 Russ Cox.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "regexp.h"
#include "memoize.h"
#include "log.h"

#include <ctype.h>

static Inst *pc; /* VM array */
static int count(Regexp*);
static void emit(Regexp*, int);

static void
Prog_assignStateNumbers(Prog *p)
{
	int i;
	for (i = 0; i < p->len; i++) {
		p->start[i].stateNum = i;
	}
}

// Transformation passes
Regexp* _transformCurlies(Regexp *r);
Regexp* _transformAltGroups(Regexp *r);
Regexp* _escapedNumsToBackrefs(Regexp *r);
Regexp* _mergeCustomCharClassRanges(Regexp *r);

/* Update this Regexp AST to make it more amenable to compilation
 *  - convert Curly to Alt-chain by expansion: A{1,3} --> A(A(A)?)?
 *  - replace Alt-chains with a "flat" AltList with one child per Alt entity
 *  - replace a CustomCharClass's CharRange chain with a flat list of CharRange's within the CCC
 *  - convert \1 to a backref
 */
Regexp*
transform(Regexp *r)
{
	Regexp *ret;

	logMsg(LOG_INFO, "Transforming regex (AST pass)");

	ret = r;
	logMsg(LOG_DEBUG, "  Curlies");
	ret = _transformCurlies(ret);
	logMsg(LOG_DEBUG, "  AltGroups");
	ret = _transformAltGroups(ret);
	logMsg(LOG_DEBUG, "  Backrefs");
	ret = _escapedNumsToBackrefs(ret);
	logMsg(LOG_DEBUG, "  CustomCharClass");
	ret = _mergeCustomCharClassRanges(ret);

	return ret;
}


void
_replaceChild(Regexp *parent, Regexp *oldChild, Regexp *newChild)
{
	if (parent->left == oldChild)
		parent->left = newChild;
    else if (parent->right == oldChild)
		parent->right = newChild;
	else
		fatal("parent had no such child");
}

static
Regexp *
_repeatPatternWithConcat(Regexp *r, int n)
{
	Regexp *ret = NULL;

	assert(n >= 1);
	if (n == 1) {
		ret = copyreg(r);
	} else {
		ret = reg(Cat, copyreg(r), NULL);
		Regexp *curr = ret;
		int i;
		for (i = 2; i < n; i++) { // Start at 2 because (a) we already used 0, and (b) final Cat is non-empty
			curr->right = reg(Cat, copyreg(r), NULL);
			curr = curr->right;
		}
		curr->right = copyreg(r);
	}

	return ret;
}

static
Regexp *
_repeatPatternWithNestedQuest(Regexp *r, int max)
{
	assert(r != NULL);
	assert(max > 0);
	Regexp *ret = NULL;

	// max may be large, e.g. x{1,4096}.
	// To avoid recursion, we'll start with the innermost and work our way outward.
	// max > 0, so we know there's at least an innermost node
	Regexp *innermost = reg(Quest, copyreg(r), NULL);

	int i;
	Regexp *prev = innermost;
	for (i = 1; i < max; i++) {
		// Given prev, the next layer is (X prev)?
		Regexp *nextInnermost = reg(Quest, reg(Cat, copyreg(r), prev), NULL);
		prev = nextInnermost;
	}
	ret = prev;

	return ret;
}

#if 0
// A{min,max} -> A|AA|AAA|...
// This works but it's not smart. You create |m-n|^2 copies of A, even worse for nesting.
static
Regexp *
_repeatPatternWithAlt(Regexp *r, int min, int max)
{
	Regexp *ret = NULL;
	assert (min <= max && min >= 0 && max >= 0);

	if (min == max) {
		ret = copyreg(r);
	} else {
		ret = reg(Alt, NULL, _repeatPatternWithConcat(r, max));
		Regexp *curr = ret;
		int i;

		for (i = max-1; i > min; i--) {
			curr->left = reg(Alt, NULL, _repeatPatternWithConcat(r, i));
			curr = curr->left;
		}
		curr->left = _repeatPatternWithConcat(r, min);
	}

	return ret;
}
#endif

/* Given A and recursively transformed A':
 *   A{2}   ->  A'A'
 *   A{1,2} ->  A'(A')?
 *   A{,2}  ->  (A'(A')?)?
 *   A{2,}  ->  A'A'A'*
 */
Regexp*
_transformCurlies(Regexp *r)
{
	switch(r->type) {
	default:
		fatal("transformCurlies: unknown type");
		return NULL;
	case Curly:
	{
		logMsg(LOG_DEBUG, "  transformCurlies: Rewriting Curly: (min %d, max %d)", r->curlyMin, r->curlyMax);
		assert(!(r->curlyMin == -1 && r->curlyMax == -1)); // reject r = a{,} 
		// r is of the form {m,n} where at most one of m and n is undefined

		// Obtain A'. Make a copy anywhere you use it.
		Regexp *A = _transformCurlies(r->left);
		// This is populated with the replacement tree
		Regexp *newR = NULL;

		Regexp *prefix = NULL;
		Regexp *suffix = NULL;

		// TODO: 
		//   2. Express A'{,n} as either A' (if n == -1) or Ques(A'.Ques(...))
		//      NB: (?:A(?:A(...)?)?)? is "tail recursive" so all of the jumps point to the same place. in-deg>1 just covers that one place.
		//      In our implementation, if capture groups are used instead, we lose this desirable property because of how in-deg is calculated.
		//   3. Re-measure |Q| etc.

		// 1. Factor out any prefix to reduce to A'{,n} 
		int prefixLen = 0;
		if (r->curlyMin > 0) {
			logMsg(LOG_DEBUG, "  transformCurlies: Factoring out prefix of length %d", r->curlyMin);
			prefixLen = r->curlyMin;
			prefix = _repeatPatternWithConcat(A, r->curlyMin);
		}

		// 2. Express A'{,n} as either A'* (if n == -1) or Ques(A'.Ques(...))
		if (r->curlyMax == -1) {
			logMsg(LOG_DEBUG, "  transformCurlies: Suffix is A*");
			suffix = reg(Star, copyreg(A), NULL);
		} else {
			int remainder = r->curlyMax - prefixLen;
			if (remainder > 0) {
				// A{,7}: Express with nested Quest
				logMsg(LOG_DEBUG, "  transformCurlies: Suffix is A{,%d}", remainder);
				suffix = _repeatPatternWithNestedQuest(A, remainder);
			} else {
				// A{5,5} == A{5}
				logMsg(LOG_DEBUG, "  transformCurlies: No suffix");
				suffix = NULL;
			}
		}

		assert(prefix != NULL || suffix != NULL);
		if (prefix == NULL) {
			newR = suffix;
		} else if (suffix == NULL) {
			newR = prefix;
		} else {
			newR = reg(Cat, prefix, suffix);
		}

		freereg(A); // We no longer need this subtree
		free(r); // We no longer need this Curly node -- should this be freereg now that copyreg is deep?
		return newR;
	}
	case Alt:
	case Cat:
		/* Binary operators -- pass the buck. */
		logMsg(LOG_DEBUG, "  curlies: Alt/Cat: passing buck");
		r->left = _transformCurlies(r->left);
		r->right = _transformCurlies(r->right);
		return r;
	case Quest:
	case Star:
	case Plus:
	case Paren:
	case CustomCharClass:
	case Lookahead:
		/* Unary operators -- pass the buck. */
		logMsg(LOG_DEBUG, "  curlies: Quest/Star/Plus/Paren/CCC/Lookahead: passing buck");
		if (r->left != NULL)
			r->left = _transformCurlies(r->left);
		return r;
	case Lit:
	case Dot:
	case CharEscape:
	case CharRange:
	case InlineZWA:
		/* Terminals */
		logMsg(LOG_DEBUG, "  curlies: ignoring terminal");
		return r;
	}

	return r;
}

int
_countAltListSize(Regexp *r)
{
	if (r->type != Alt) {
		// Base case -- some child of an Alt
		return 1;
	}
	// Left-recursive: A|B|C -> Alt(Alt(A|B), C)
	return 1 + _countAltListSize(r->left);
}

// Fill the children array in left-to-right order
// Returns the smallest unused index
int
_fillAltChildren(Regexp *r, Regexp **children, int i)
{
	if (r->type == Alt) {
		// Recursively populate the left children first
		int next = _fillAltChildren(r->left, children, i);
		// Now populate right child
		assert(r->right->type != Alt); // I think?
		children[next] = r->right;
		free(r); // We don't need this Reg node anymore -- the left and right have been copied out to the parent already
		return next + 1;
	} else {
		// End of the recursion
		children[i] = r;
		return i + 1;
	}
}

Regexp*
_transformAltGroups(Regexp *r)
{
	Regexp *altList = NULL;
	int groupSize = 0, i = 0;

	switch(r->type) {
	default:
		fatal("transformAltGroups: unknown type");
		return NULL;
	case Alt:
		/* Prepare an AltList node */
		logMsg(LOG_DEBUG, "Converting an Alt to an AltList");
		groupSize = _countAltListSize(r);
		logMsg(LOG_DEBUG, "  groupSize %d", groupSize);
		assert(groupSize >= 2);

		altList = mal(sizeof(*altList));
		altList->type = AltList;
		altList->children = mal(groupSize * sizeof(altList));
		altList->arity = groupSize;
		logMsg(LOG_DEBUG, "  Populating children array");
		_fillAltChildren(r, altList->children, 0);

		/* Optimize the children */
		logMsg(LOG_DEBUG, "  Passing buck to children");
		for (i = 0; i < groupSize; i++) {
			altList->children[i] = _transformAltGroups(altList->children[i]);
		}

		return altList;
	case Cat:
		/* Binary operator -- pass the buck. */
		logMsg(LOG_DEBUG, "  altGroups: Cat: passing buck");
		r->left = _transformAltGroups(r->left);
		r->right = _transformAltGroups(r->right);
		return r;
	case Quest:
	case Star:
    case Plus:
	case Paren:
	case CustomCharClass:
	case Lookahead:
	case Curly:
		/* Unary operators -- pass the buck. */
		logMsg(LOG_DEBUG, "  altGroups: Quest/Star/Plus/Paren/CCC/Lookahead/Curly: passing buck");
		if (r->left != NULL)
			r->left = _transformAltGroups(r->left);
		return r;
	case Lit:
	case Dot:
	case CharEscape:
	case CharRange:
	case InlineZWA:
		/* Terminals */
		logMsg(LOG_DEBUG, "  altGroups: ignoring terminal");
		return r;
	}
	return r;
}

Regexp *
_escapedNumsToBackrefs(Regexp *r)
{
	char s[2];
	int i, n;
	switch(r->type) {
	default:
		logMsg(LOG_ERROR, "type %d", r->type);
		fatal("escapedNumsToBackrefs: unknown type");
		return NULL;
	case CharEscape:
		s[0] = r->ch;
		s[1] = '\0';
		n = atoi(s);
		if (0 < n && n < 10) {
			/* Change inline */
			r->type = Backref;
			r->cgNum = n;
		}
		return r;
	case AltList:
		/* *-ary operator -- pass the buck. */
		for (i = 0; i < r->arity; i++) {
			r->children[i] = _escapedNumsToBackrefs(r->children[i]);
		}
		return r;
	case Alt:
	case Cat:
		/* Binary operator -- pass the buck. */
		logMsg(LOG_DEBUG, "  backrefs: Cat: passing buck");
		r->left = _escapedNumsToBackrefs(r->left);
		r->right = _escapedNumsToBackrefs(r->right);
		return r;
	case Quest:
	case Star:
    case Plus:
	case Paren:
	case Lookahead:
	case Curly:
		/* Unary operators -- pass the buck. */
		logMsg(LOG_DEBUG, "  backrefs: Quest/Star/Plus/Paren/CCC/Lookahead/Curly: passing buck");
		r->left = _escapedNumsToBackrefs(r->left);
		return r;
	case Lit:
	case Dot:
	case CustomCharClass:
	case InlineZWA:
		/* Terminals */
		logMsg(LOG_DEBUG, "  backrefs: ignoring terminal");
		return r;
	}
}

int
_countCCCNRanges(Regexp *r)
{
	if (r == NULL)
		return 0;

	if (r->type != CharRange)
		fatal("countCCCNRanges: unexpected type");

	int nChildren = 1;
	if (r->left != NULL) {
		// Left-recursive: A|B|C -> Alt(Alt(A|B), C)
		nChildren += _countCCCNRanges(r->left);
	}
	return nChildren;
}

// Fill the children array in left-to-right order
// Returns the smallest unused index
int
_fillCCCChildren(Regexp *r, Regexp **children, int i)
{
	if (r == NULL)
		return 0;

	if (r->type != CharRange)
		fatal("fillCCCChildren: unexpected type");

	int next = i;
	if (r->left != NULL) {
		// Recursively populate the left children first
		next = _fillCCCChildren(r->left, children, i);
		r->left = NULL;
	}
	// Now populate "right child" -- the node itself
	children[next] = r;
	return next + 1;
}

Regexp*
_mergeCustomCharClassRanges(Regexp *r)
{
	int i;
	int groupSize = 0;

	switch(r->type) {
	default:
		logMsg(LOG_ERROR, "type %d", r->type);
		fatal("mergeCustomCharClassRanges: unknown type");
		return NULL;
	case CustomCharClass:
		logMsg(LOG_DEBUG, "In-place updating a CCC to have all its children in one place");
		groupSize = _countCCCNRanges(r->left);
		logMsg(LOG_DEBUG, "  groupSize %d", groupSize);

		r->children = mal(groupSize * sizeof(Regexp *));
		r->arity = groupSize;
		logMsg(LOG_DEBUG, "  Populating children array");
		_fillCCCChildren(r->left, r->children, 0);

		r->mergedRanges = 1;
		r->left = NULL;
		r->right = NULL;

		return r;
	case AltList:
		/* *-ary operator -- pass the buck. */
		for (i = 0; i < r->arity; i++) {
			r->children[i] = _mergeCustomCharClassRanges(r->children[i]);
		}
		return r;
	case Alt:
	case Cat:
		/* Binary operator -- pass the buck. */
		logMsg(LOG_DEBUG, "  mergeCCC: Cat: passing buck");
		r->left = _mergeCustomCharClassRanges(r->left);
		r->right = _mergeCustomCharClassRanges(r->right);
		return r;
	case Quest:
	case Star:
    case Plus:
	case Paren:
	case Lookahead:
	case Curly:
		/* Unary operators -- pass the buck. */
		logMsg(LOG_DEBUG, "  mergeCCC: Quest/Star/Plus/Paren/CCC/Lookahead/Curly: passing buck");
		r->left = _mergeCustomCharClassRanges(r->left);
		return r;
	case Lit:
	case Dot:
	case CharEscape:
	case Backref:
	case InlineZWA:
		/* Terminals */
		logMsg(LOG_DEBUG, "  mergeCCC: ignoring terminal");
		return r;
	}
	return r;
}

// Compile into a Prog
Prog*
compile(Regexp *r, int memoMode, int memoEncoding, int *rleValues, int rleValuesLength, int singleRleK)
{
	int i, n;
	Prog *p;

	n = count(r) + 1;

	p = mal(sizeof *p + n*sizeof p->start[0]);
	p->start = (Inst*)(p+1);
	pc = p->start;
	if (memoEncoding == ENCODING_RLE_TUNED) {
		// for (i = 0; i < n; i++) {
		// 	if (singleRleK != NULL){
		// 		p->start[i].memoInfo.visitInterval = singleRleK;
		// 	} else{
		// 		if (i < rleValuesLength){
		// 			p->start[i].memoInfo.visitInterval = rleValues[i];	
		// 		} else {
		// 			p->start[i].memoInfo.visitInterval = 1; /* A good default */
		// 		}
		// 	}
		// }
		for (i = 0; i < n; i++) {
			p->start[i].memoInfo.visitInterval = singleRleK; /* A good default */
		}
	} else {
		for (i = 0; i < n; i++) {
			p->start[i].memoInfo.visitInterval = 1; /* A good default */
		}
	}
	
	emit(r, memoMode);
	pc->opcode = Match;
	pc++;
	p->len = pc - p->start;
	p->eolAnchor = r->eolAnchor;

	Prog_assignStateNumbers(p);
	return p;
}

// How many instructions does r need?
static int
count(Regexp *r)
{
	int _count = 0, i;
	switch(r->type) {
	default:
		fatal("count: unknown type");
	case Alt:
		return 2 + count(r->left) + count(r->right);
    case AltList:
		_count = 0;
		for (i = 0; i < r->arity; i++) {
			// Each branch adds 1 jump
			_count += count(r->children[i]) + 1;
		}
		// Need a SplitMany as well
		return 1 + _count;
	case Cat:
		return count(r->left) + count(r->right);
	case Lit:
	case Dot:
	case CharEscape:
	case CustomCharClass:
	case Backref:
	case InlineZWA:
		return 1;
	case Paren:
		return 2 + count(r->left);
	case Quest:
		return 1 + count(r->left);
	case Star:
		return 2 + count(r->left);
	case Plus:
		return 1 +  count(r->left);
	case Lookahead:
		return 2 +  count(r->left); /* ZWA + RecursiveMatch */
	}
}

#if 0
// Determine size of simple languages for r
// Recursively populates sub-patterns
// TODO This is a WIP. Do not use this.
static void
Regexp_calcLLI(Regexp *r)
{
	int i, j;
	switch(r->type) {
	default:
		return;
		fatal("calcLLI: unknown type");
	case AltList:
	case CustomCharClass:
	case CharRange:
		return;
	case Alt:
		Regexp_calcLLI(r->left);
		Regexp_calcLLI(r->right);
		
		/* Combine: A giant OR */
		r->lli.nLanguageLengths = 0;
		for (i = 0; i < r->left->lli.nLanguageLengths; i++) {
			lli_addEntry(&r->lli, r->left->lli.languageLengths[i]);
		}
		for (i = 0; i < r->right->lli.nLanguageLengths; i++) {
			lli_addEntry(&r->lli, r->right->lli.languageLengths[i]);
		}

		logMsg(LOG_VERBOSE, "LLI: Alt");
		lli_print(&r->lli);
		break;
	case Cat:
		Regexp_calcLLI(r->left);
		Regexp_calcLLI(r->right);

		/* Combine: Cartesian product */
		r->lli.nLanguageLengths = 0;
		for (i = 0; i < r->left->lli.nLanguageLengths; i++) {
			for (j = 0; j < r->right->lli.nLanguageLengths; j++) {
				lli_addEntry(&r->lli, r->left->lli.languageLengths[i] + r->right->lli.languageLengths[j]);
			}
		}

		logMsg(LOG_VERBOSE, "LLI: Cat");
		lli_print(&r->lli);
		break;
	case Lit:
	case Dot:
	case CharEscape:
		r->lli.nLanguageLengths = 1;
		r->lli.languageLengths[0] = 1;

		logMsg(LOG_VERBOSE, "LLI: Lit,Dot,CharEscape");
		lli_print(&r->lli);
		break;
	case Paren:
		Regexp_calcLLI(r->left);
		r->lli = r->left->lli;

		logMsg(LOG_VERBOSE, "LLI: Paren");
		lli_print(&r->lli);
		break;
	case Quest:
		Regexp_calcLLI(r->left);
		r->lli = r->left->lli;
		lli_addEntry(&r->lli, 0);

		logMsg(LOG_VERBOSE, "LLI: Quest:");
		lli_print(&r->lli);
		break;
	case Star:
		Regexp_calcLLI(r->left);
		r->lli = r->left->lli;
		lli_addEntry(&r->lli, 0);

		logMsg(LOG_VERBOSE, "LLI: Star");
		lli_print(&r->lli);
		break;
	case Plus:
		Regexp_calcLLI(r->left);
		r->lli = r->left->lli;

		logMsg(LOG_VERBOSE, "LLI: Plus");
		lli_print(&r->lli);
		break;
	}
}

static void
printre_VI(Regexp *r)
{
	return;

	switch(r->type) {
	default:
		printf("???");
		break;
	
	case Alt:
		printf("Alt-%d(", r->visitInterval);
		printre_VI(r->left);
		printf(", ");
		printre_VI(r->right);
		printf(")");
		break;

	case Cat:
		printf("Cat-%d(", r->visitInterval);
		printre_VI(r->left);
		printf(", ");
		printre_VI(r->right);
		printf(")");
		break;
	
	case Lit:
		printf("Lit(%c)", r->ch);
		break;
	
	case Dot:
		printf("Dot");
		break;

	case CharEscape:
		printf("Esc(%c)", r->ch);
		break;

	case Paren:
		printf("Paren-%d(%d, ", r->visitInterval, r->n);
		printre_VI(r->left);
		printf(")");
		break;
	
	case Star:
		if(r->n)
			printf("Ng");
		printf("Star-%d(", r->visitInterval);
		printre_VI(r->left);
		printf(")");
		break;
	
	case Plus:
		if(r->n)
			printf("Ng");
		printf("Plus-%d(", r->visitInterval);
		printre_VI(r->left);
		printf(")");
		break;
	
	case Quest:
		if(r->n)
			printf("Ng");
		printf("Quest-%d(", r->visitInterval);
		printre_VI(r->left);
		printf(")");
		break;
	}
}

// Determine visit intervals for r
// Call after all LLI are known
// Recursively populates sub-patterns
static void
Regexp_calcVisitInterval(Regexp *r)
{
	switch(r->type) {
	default:
		return;
		fatal("calcVI: unknown type");
	case AltList:
	case CustomCharClass:
		return;
	case Alt:
		Regexp_calcVisitInterval(r->left);
		Regexp_calcVisitInterval(r->right);

		/*
		r->visitInterval = leastCommonMultiple2(r->left->visitInterval, \
			r->right->visitInterval);
		*/
		r->visitInterval = leastCommonMultiple2(
			lli_smallestUniversalPeriod(&r->left->lli),
			lli_smallestUniversalPeriod(&r->right->lli)
		);

		//r->visitInterval = r->left->visitInterval + r->right->visitInterval;
		//r->visitInterval = lli_smallestUniversalPeriod(&r->left->lli) + lli_smallestUniversalPeriod(&r->right->lli);

		logMsg(LOG_VERBOSE, "Alt: VI %d", r->visitInterval);
		break;
	case Cat:
		Regexp_calcVisitInterval(r->left);
		Regexp_calcVisitInterval(r->right);

#if 0
		/* This helps dotStar-1 */
		r->visitInterval = leastCommonMultiple2(
			lli_smallestUniversalPeriod(&r->left->lli), // <-- This
			lli_smallestUniversalPeriod(&r->right->lli)
		);

		/* This helps concat-1 */
		r->visitInterval = leastCommonMultiple2(
			r->left->visitInterval,
			lli_smallestUniversalPeriod(&r->right->lli)
		);

		/* Experimental.
		 * TODO This is a hack. Concatenation SUPs get longer and longer,
		 * but we only need to care when we reach a vertex with its own VI? */
		r->visitInterval = leastCommonMultiple2(
			leastCommonMultiple2(r->left->visitInterval, lli_smallestUniversalPeriod(&r->left->lli)),
			leastCommonMultiple2(r->right->visitInterval, lli_smallestUniversalPeriod(&r->right->lli))
		);

		// TODO Experimenting.
		if (r->right->visitInterval > 1) {
		} else{

		}

		/* Right incurs intervals from left. */
		r->right->visitInterval = leastCommonMultiple2(
			r->left->visitInterval,
			lli_smallestUniversalPeriod(&r->right->lli)
		);

		/* Whole takes on only left. */
		r->visitInterval = r->right->visitInterval;
		
		r->visitInterval = leastCommonMultiple2(
			//leastCommonMultiple2(r->left->visitInterval, lli_smallestUniversalPeriod(&r->left->lli)),
			//lli_smallestUniversalPeriod(&r->left->lli),
			r->left->visitInterval,
			//leastCommonMultiple2(r->right->visitInterval, lli_smallestUniversalPeriod(&r->right->lli))
			//r->right->visitInterval
			lli_smallestUniversalPeriod(&r->right->lli)
		);
#endif

		// B will be visited at intervals of left 
		//   e.g. Cat(A, B)
		// B may be visited at intervals of itself
		//   e.g. Cat(A, Star(B))
		r->right->visitInterval = leastCommonMultiple2(
			lli_smallestUniversalPeriod(&r->left->lli),
			lli_smallestUniversalPeriod(&r->right->lli)
		);
		// Propagate this down through any Parens to the thing they are wrapping.
		if (r->right->type == Paren) {
			Regexp *tmpRight = NULL;
			int vi = r->right->visitInterval;

			logMsg(LOG_VERBOSE, "Propagating vi %d past Parens", vi);
			tmpRight = r->right;
			while (tmpRight->type == Paren) {
				tmpRight->visitInterval = vi;
				tmpRight = tmpRight->left;
			}
			// We have our target, the first non-paren entity.
			// This also takes on the VI.
			tmpRight->visitInterval = vi;
		}

		// The Cat node takes on both.
		//  - It can be visited after repetitions of A and B
		//    (e.g. Star(Cat(A, B)) )
		r->visitInterval = leastCommonMultiple2(
			r->left->visitInterval,
			r->right->visitInterval
		);

		logMsg(LOG_VERBOSE, "Cat: VI self %d l->vi %d l->SUP %d r->vi %d r->SUP %d", r->visitInterval, r->left->visitInterval, lli_smallestUniversalPeriod(&r->left->lli), r->right->visitInterval, lli_smallestUniversalPeriod(&r->right->lli));
		if (r->left->type == Paren) {
			logMsg(LOG_VERBOSE, "Cat: L = Paren");
		}
		if (r->right->type == Paren) {
			logMsg(LOG_VERBOSE, "Cat: R = Paren");
		}
		break;
	case Lit:
	case Dot:
	case CharEscape:
		r->visitInterval = 1;
		break;
	case Paren:
		Regexp_calcVisitInterval(r->left);
		r->visitInterval = r->left->visitInterval;

		logMsg(LOG_VERBOSE, "Paren: VI %d", r->visitInterval);
		break;
	case Quest:
	case Star:
	case Plus:
		Regexp_calcVisitInterval(r->left);
		r->visitInterval = lli_smallestUniversalPeriod(&r->left->lli);
		logMsg(LOG_VERBOSE, "Quest|Star|Plus: VI %d", r->visitInterval);
		break;
	}
}
#endif

static void
_emitRegexpCharEscape2InstCharRange(Regexp *r, InstCharRange *instCR)
{
	if (r->type != CharEscape) {
		assert(!"emitrcr2instCR: Unexpected type");
	}

	switch (r->ch) {
	case 's':
	case 'S':
		/* space, newline, tab, vertical wsp, a few others */
		instCR->lows[0] = 9; instCR->highs[0] = 13;
		instCR->lows[1] = 28; instCR->highs[1] = 32;
		instCR->count = 2;
		instCR->invert = isupper(r->ch);
		return;
	case 'w':
	case 'W':
		/* a-z A-Z 0-9 */
		instCR->lows[0] = 97; instCR->highs[0] = 122;
		instCR->lows[1] = 65; instCR->highs[1] = 90;
		instCR->lows[2] = 48; instCR->highs[2] = 57;
		instCR->count = 3;
		instCR->invert = isupper(r->ch);
		return;
	case 'd':
	case 'D':
		/* 0-9 */
		instCR->lows[0] = 48; instCR->highs[0] = 57;
		instCR->count = 1;
		instCR->invert = isupper(r->ch);
		return;
	/* Not a built-in CC */
	// Handle special escape sequences
	case 'r': // UNIX-style!
	case 'n':
		instCR->lows[0] = '\n'; instCR->highs[0] = '\n';
		instCR->count = 1;
		return;
	case 't':
		instCR->lows[0] = '\t'; instCR->highs[0] = '\t';
		instCR->count = 1;
		return;
	case 'f':
		instCR->lows[0] = '\f'; instCR->highs[0] = '\f';
		instCR->count = 1;
		return;
	case 'v':
		instCR->lows[0] = '\v'; instCR->highs[0] = '\v';
		instCR->count = 1;
		return;
	// By default, treat it as "not an escape": \a is just a literal "a"
	default:
		instCR->lows[0] = r->ch; instCR->highs[0] = r->ch;
		instCR->count = 1;
		return;
	}
}

static void
_emitRegexpCharRange2Inst(Regexp *r, Inst *inst)
{
	InstCharRange *next = &inst->charRanges[ inst->charRangeCounts ];
	switch (r->type) {
    default:
		assert(!"emitrcr2int: Unexpected type");
	case CharEscape: /* e.g. \w (built-in CC) or \a (nothing) */
		_emitRegexpCharEscape2InstCharRange(r, next);
		break;
	case CharRange:
		switch (r->ccLow->type) {
		case Lit: /* 'a-z' */
			assert(r->ccHigh->type == Lit); /* 'a', or 'a-z' (but not 'a-\w') */
			next->lows[0] = r->ccLow->ch; next->highs[0] = r->ccHigh->ch;
			next->count = 1;
			break;
		case CharEscape:
			assert(r->ccLow->ch == r->ccHigh->ch); // '\w', not '\w-\s'
			_emitRegexpCharEscape2InstCharRange(r->ccLow, next);
			break;
		default:
			assert(!"emitrcr2int: CharRange: Unexpected child type");
		}
		break;
	}
}

/* Populate pc for r
 *   emit() produces instructions corresponding to r
 *   and saves them into the global pc array
 * 
 *   Instructions are emitted sequentially into pc,
 *     whose size is calculated by walking r in count()
 *     and adding values based on the number of states emit()'d
 *     by each op type
 * 
 *   emit() is defined recursively
 * 
 * 	 Each call to emit() starts at the largest unused pc
 *   
 *   During simulation,
 *     - some pc's can skip around (Jmp, Split)
 *     - others just advance the pc to the next (adjacent) instruction
 * 
 *   We use memoMode here because Alt and Star are compiled into similar-looking opcodes
 *   Easiest to handle MEMO_LOOP_DEST during emit().
 * 
 *   Call after Regexp_calcLLI.
 */ 
static void
emit(Regexp *r, int memoMode)
{
	Inst *p1, *p2, *t, **t2;
	int i;

	switch(r->type) {
	default:
		fatal("emit: unknown type");

	case Alt:
		pc->opcode = Split;
		p1 = pc++;
		p1->x = pc;
		emit(r->left, memoMode);
		pc->opcode = Jmp;
		p2 = pc++;
		p1->y = pc;
		emit(r->right, memoMode);
		p2->x = pc;
		break;

	case AltList:
		pc->opcode = SplitMany;
		pc->arity = r->arity;
		pc->edges = mal(r->arity * sizeof(Inst **));

		/* The Jmp nodes associated with each branch */
		t2 = mal(r->arity * sizeof(Inst **));

		/* Emit the branches */
		p1 = pc++;
		p1->x = pc;
		for (i = 0; i < r->arity; i++) {
			/* Emit a branch */
			p1->edges[i] = pc;
			emit(r->children[i], memoMode);
			/* Emit a Jmp node and save it so we can set its destination once we exhaust the AltList */
			pc->opcode = Jmp;
			t2[i] = pc;
			/* Ready for the next branch */
			pc++;
		}

		/* Revisit the Jmp nodes and set the destinations */
		for (i = 0; i < r->arity; i++) {
			t2[i]->x = pc;
		}
		free(t2);

		break;

	case Cat:
		p1 = pc;
		emit(r->left, memoMode);
		p2 = pc;
		emit(r->right, memoMode);

		break;
	
	case Lit:
		pc->opcode = Char;
		pc->c = r->ch;
		pc++;
		break;

	case CustomCharClass:
		assert(r->mergedRanges);
		pc->opcode = CharClass;
		if (r->arity+1 > nelem(pc->charRanges)) // +1: space for a dash if needed
			fatal("Too many ranges in char class");

		pc->charRangeCounts = 0;
		for (i = 0; i < r->arity; i++) {
			// This doesn't really emit, it's actually populating pc fields
			_emitRegexpCharRange2Inst(r->children[i], pc);
			pc->charRangeCounts++;
		}
		if (r->plusDash) {
			pc->charRanges[pc->charRangeCounts].lows[0] = '-';
			pc->charRanges[pc->charRangeCounts].highs[0] = '-';
			pc->charRanges[pc->charRangeCounts].count = 1;

			pc->charRangeCounts++;
		}
		pc->invert = r->ccInvert;
		pc++;
		break;

	case CharEscape:
		pc->opcode = CharClass;

		// Fill in the pc details
		_emitRegexpCharRange2Inst(r, pc);
		pc->charRangeCounts = 1;
		pc++;
		break;
	
	case Dot:
		pc++->opcode = Any;
		break;

	case Paren:
		pc->opcode = Save;
		pc->n = 2*r->n;

		pc++;
		emit(r->left, memoMode);
		pc->opcode = Save;
		pc->n = 2*r->n + 1;

		pc++;
		break;
	
	case Quest:
		pc->opcode = Split;
		p1 = pc++;
		p1->x = pc;
		emit(r->left, memoMode);
		p1->y = pc;
		if(r->n) {	// non-greedy
			t = p1->x;
			p1->x = p1->y;
			p1->y = t;
		}
		break;

	case Star:
		pc->opcode = Split;
		p1 = pc++;
		p1->x = pc;
		emit(r->left, memoMode);
		pc->opcode = Jmp;
		pc->x = p1; /* Back-edge */
		pc++;
		p1->y = pc;
		if(r->n) {	// non-greedy
			t = p1->x;
			p1->x = p1->y;
			p1->y = t;
		}
		break;

	case Plus:
		p1 = pc;
		emit(r->left, memoMode);
		pc->opcode = Split;
		pc->x = p1; /* Back-edge */
		p2 = pc;
		pc++;
		p2->y = pc;
		if(r->n) {	// non-greedy
			t = p2->x;
			p2->x = p2->y;
			p2->y = t;
		}
		break;

	case Backref:
		pc->opcode = StringCompare;
		pc->cgNum = r->cgNum;
		pc++;
		break;

	case Lookahead:
		pc->opcode = RecursiveZeroWidthAssertion;
		pc++;
		emit(r->left, memoMode);
		pc->opcode = RecursiveMatch;
		pc++;
		break;

	case InlineZWA:
		pc->opcode = InlineZeroWidthAssertion;
		pc->c = r->ch;
		pc++;
		break;
	}
}

// This function is used in simulation, but is most appropriately defined here.
int
usesBackreferences(Prog *prog)
{
  Inst *pc;
  int i;

  for (i = 0, pc = prog->start; i < prog->len; i++, pc++) {
    if (pc->opcode == StringCompare) {
      return 1;
    }
  }
  return 0;
}

char* printAllCharRanges(const Inst *inst) {
    // printf("CharRanges:\n");
	char* result = (char*)malloc(100 * sizeof(char));
    if (result == NULL) {
        printf("Memory allocation failed\n");
        exit(1);
    }
    for (int i = 0; i < inst->charRangeCounts; i++) {
        // printf("CharRange #%d:\n", i + 1);
		if (inst->invert || inst->charRanges[i].invert) {
            strcat(result, "^");
        }
        for (int j = 0; j < inst->charRanges[i].count; j++) {
            // Convert the lows and highs to characters and concatenate them
            char low = inst->charRanges[i].lows[j];
            char high = inst->charRanges[i].highs[j];
			// printf("%d: %d %d \n", j, low, high);
            // char range[6];

            sprintf(result + strlen(result), "%d-%d ", low, high);
			// printf("%s \n", result);
            // strcat(result, range);
        }
    }
	// printf("\n%s\n", result);
	return result;
}

void
printprog(Prog *p)
{
	Inst *pc, *e;
	int i;
	
	pc = p->start;
	e = p->start + p->len;
	printf("BEGIN\n");
	for(; pc < e; pc++) {
		switch(pc->opcode) {
		default:
			fatal("printprog: unknown opcode");
		case StringCompare:
			printf("%2d. stringcompare %d (memo? %d -- state %d, visitInterval %d)\n", (int)(pc-p->start), pc->cgNum, pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			break;
		case Split:
			printf("%2d. split %d, %d (memo? %d -- state %d, visitInterval %d)\n", (int)(pc-p->start), (int)(pc->x-p->start), (int)(pc->y-p->start), pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			//printf("%2d. split %d, %d\n", (int)(pc->stateNum), (int)(pc->x->stateNum), (int)(pc->y->stateNum));
			break;
		case SplitMany:
			printf("%2d. splitmany ", (int) (pc - p->start));
			for (i = 0; i < pc->arity; i++) {
				printf("%d", (int) (pc->edges[i]-p->start));
				if (i + 1 < pc->arity)
					printf(", ");
			}
			printf("  (memo? %d -- state %d, visitInterval %d)\n", pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			//printf("%2d. split %d, %d\n", (int)(pc->stateNum), (int)(pc->x->stateNum), (int)(pc->y->stateNum));
			break;
		case Jmp:
			printf("%2d. jmp %d (memo? %d -- state %d, visitInterval %d)\n", (int)(pc-p->start), (int)(pc->x-p->start), pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			//printf("%2d. jmp %d\n", (int)(pc->stateNum), (int)(pc->x->stateNum));
			break;
		case Char:
			printf("%2d. char %d (memo? %d -- state %d, visitInterval %d)\n", (int)(pc-p->start), pc->c, pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			//printf("%2d. char %c\n", (int)(pc->stateNum), pc->c);
			break;
		case Any:
			printf("%2d. any (memo? %d -- state %d, visitInterval %d)\n", (int)(pc-p->start), pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			//printf("%2d. any\n", (int)(pc->stateNum));
			break;
		case InlineZeroWidthAssertion:
			printf("%2d. inlineZWA %c (memo? %d -- state %d)\n", (int)(pc-p->start), pc->c, pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum);
			//printf("%2d. any\n", (int)(pc->stateNum));
			break;
		case RecursiveZeroWidthAssertion:
			printf("%2d. recursizeZWA \n", (int)(pc-p->start));
			break;
		case RecursiveMatch:
			printf("%2d. recursivematch\n", (int)(pc-p->start));
			break;
		case CharClass:
			// printAllCharRanges(pc);
			printf("%2d. charClass %s (memo? %d -- state %d, visitInterval %d)\n", (int)(pc-p->start), printAllCharRanges(pc),  pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			//printf("%2d. any\n", (int)(pc->stateNum));
			break;
		case Match:
			printf("%2d. match (memo? %d -- state %d, visitInterval %d)\n", (int)(pc-p->start), pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			//printf("%2d. match\n", (int)(pc->stateNum));
			break;
		case Save:
			printf("%2d. save %d (memo? %d -- state %d, visitInterval %d)\n", (int)(pc-p->start), pc->n, pc->memoInfo.shouldMemo, pc->memoInfo.memoStateNum, pc->memoInfo.visitInterval);
			//printf("%2d. save %d\n", (int)(pc->stateNum), pc->n);
			break;
		}
	}
	printf("END\n");
}

static void Prog_unmarkAll(Prog *p)
{
	int i = 0;
	for (i = 0; i < p->len; i++) {
		p->start[i].startMark = 0;
		p->start[i].visitMark = 0;
	}
}

static int Inst_couldStartLoop(Inst *inst)
{
    // For the infinite loop check, we check if there's a loop to a branch.
	// Only branches can introduce a back-edge -- START a loop.
	switch (inst->opcode) {
    case Jmp:
	case Split:
	case SplitMany:
		return 1;
	default:
		return 0;
	}
}

// Return non-zero if we form a cycle, starting from stateNum, without consuming a character
// Uses a recursive DFS. Will blow the stack on large curlies...
static int Prog_epsilonClosure(Prog *p, int stateNum, int start /* True if first call */)
{
	int i = 0;
	Inst *curr = &p->start[stateNum];

	logMsg(LOG_DEBUG, "  epsilonClosure: instr %d", stateNum);
	if (curr->startMark) {
		logMsg(LOG_DEBUG, "  infinite loop found: returned to instr %d", stateNum);
		return 1;
	} else if (curr->visitMark) {
		logMsg(LOG_DEBUG, "  visited instr %d before, nothing more to mark here", stateNum);
		return 0;
	}

	if (start) {
		curr->startMark = 1;
	} else {
		curr->visitMark = 1;
	}

	switch(curr->opcode) {
	case Jmp:
		return Prog_epsilonClosure(p, curr->x->stateNum, 0);
	case Split:
		return Prog_epsilonClosure(p, curr->x->stateNum, 0) ? 1 : Prog_epsilonClosure(p, curr->y->stateNum, 0);
	case SplitMany:
		for (i = 0; i < curr->arity; i++) {
			if (Prog_epsilonClosure(p, curr->edges[i]->stateNum, 0))
				return 1;
		}	
		return 0;
    case Char:
	case Match:
	case Any:
	case CharClass:
		return 0;
	case Save:
		// Costs 0, so skip over
		return Prog_epsilonClosure(p, stateNum + 1, 0);
	case StringCompare:
		return 0; // TODO This requires a more sophisticated analysis. (.)?\1 can match the empty string
	case InlineZeroWidthAssertion:
		// InlineZWA costs 0, so skip over
		return Prog_epsilonClosure(p, stateNum + 1, 0);
	case RecursiveZeroWidthAssertion:
	{
		// RecursiveZWA costs 0, so skip over
		// Nesting is verboten
		while (curr->opcode != RecursiveMatch) curr++;
		return Prog_epsilonClosure(p, curr->stateNum + 1, 0);
	}
	case RecursiveMatch:
		// Nothing to do here, we'll explore this from another starting vertex
		return 0;
	default:
		fatal("Unknown inst type");
	}

	fatal("Never reached");
	return 0;
}

void Prog_assertNoInfiniteLoops(Prog *p)
{
	int i = 0;
	for (i = 0; i < p->len; i++ ) {
		if (Inst_couldStartLoop(&p->start[i])) {
			Prog_unmarkAll(p);

			logMsg(LOG_DEBUG, "  check for no infinite loops: starting from instr %d", i);
			if (Prog_epsilonClosure(p, i, 1)) {
				logMsg(LOG_DEBUG, "Found infinite loop from instr %d. Unsupported regex", i);
				fatal("'syntax error': infinite loop possible due to nested *s like (a*)*");
			}
		}
	}

	logMsg(LOG_DEBUG, "No infinite loops found");
}