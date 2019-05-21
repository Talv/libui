// 27 february 2018
// TODO get rid of the need for this (it temporarily silences noise so I can find actual build issues)
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include "timer.h"
#include "testing.h"
#include "testingpriv.h"

struct defer {
	void (*f)(testingT *, void *);
	void *data;
	struct defer *next;
};

#ifdef _MSC_VER
// Microsoft defines jmp_buf with a __declspec(align()), and for whatever reason, they have a warning that triggers when you use that for any reason, and that warning is enabled with /W4
// Silence the warning; it's harmless.
#pragma warning(push)
#pragma warning(disable: 4324)
#endif

struct testingT {
	// set at test creation time
	const char *name;
	void (*f)(testingT *, void *);
	void *data;

	// for sorting tests in a set; not used by subtests
	const char *file;
	long line;

	// test status
	int failed;
	int skipped;
	int returned;
	jmp_buf returnNowBuf;

	// deferred functions
	struct defer *defers;
	int defersRun;

	// execution options
	testingOptions opts;

	// output
	testingprivOutbuf *outbuf;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

static void initTest(testingT *t, const char *name, void (*f)(testingT *, void *), void *data, const char *file, long line)
{
	memset(t, 0, sizeof (testingT));
	t->name = name;
	t->f = f;
	t->data = data;
	t->file = file;
	t->line = line;
}

struct testingSet {
	testingprivArray tests;
};

static testingSet mainTests = { testingprivArrayStaticInit(testingT, 32, "testingT[]") };

void testingprivSetRegisterTest(testingSet **pset, const char *name, void (*f)(testingT *, void *), void *data, const char *file, long line)
{
	testingSet *set;
	testingT *t;

	set = &mainTests;
	if (pset != NULL) {
		set = *pset;
		if (set == NULL) {
			set = testingprivNew(testingSet);
			testingprivArrayInit(set->tests, testingT, 32, "testingT[]");
			*pset = set;
		}
	}
	t = (testingT *) testingprivArrayAppend(&(set->tests), 1);
	initTest(t, name, f, data, file, line);
}

static int testcmp(const void *a, const void *b)
{
	const testingT *ta = (const testingT *) a;
	const testingT *tb = (const testingT *) b;
	int ret;

	ret = strcmp(ta->file, tb->file);
	if (ret != 0)
		return ret;
	if (ta->line < tb->line)
		return -1;
	if (ta->line > tb->line)
		return 1;
	return 0;
}

static void runDefers(testingT *t)
{
	struct defer *d;

	if (t->defersRun)
		return;
	t->defersRun = 1;
	for (d = t->defers; d != NULL; d = d->next)
		(*(d->f))(t, d->data);
}

static const testingOptions defaultOptions = {
	.Verbose = 0,
};

static int testingprivTRun(testingT *t, testingprivOutbuf *parentbuf)
{
	const char *status;
	timerTime start, end;
	char timerstr[timerDurationStringLen];
	int printStatus;

	if (t->opts.Verbose)
		testingprivOutbufPrintf(parentbuf, "=== RUN   %s\n", t->name);
	t->outbuf = testingprivNewOutbuf();

	start = timerMonotonicNow();
	if (setjmp(t->returnNowBuf) == 0)
		(*(t->f))(t, t->data);
	end = timerMonotonicNow();
	t->returned = 1;
	runDefers(t);

	printStatus = t->opts.Verbose;
	status = "PASS";
	if (t->failed) {
		status = "FAIL";
		printStatus = 1;			// always print status on failure
	} else if (t->skipped)
		// note that failed overrides skipped
		status = "SKIP";
	timerDurationString(timerTimeSub(end, start), timerstr);
	if (printStatus) {
		testingprivOutbufPrintf(parentbuf, "--- %s: %s (%s)\n", status, t->name, timerstr);
		testingprivOutbufAppendOutbuf(parentbuf, t->outbuf);
	}

	testingprivOutbufFree(t->outbuf);
	t->outbuf = NULL;
	return t->failed;
}

// TODO rename all options to opts and all format to fmt
static void testingprivSetRun(testingSet *set, const testingOptions *opts, testingprivOutbuf *outbuf, int *anyFailed)
{
	size_t i;
	testingT *t;

	testingprivArrayQsort(&(set->tests), testcmp);
	t = (testingT *) (set->tests.buf);
	for (i = 0; i < set->tests.len; i++) {
		t->opts = *opts;
		if (testingprivTRun(t, outbuf) != 0)
			*anyFailed = 1;
		t++;
	}
}

void testingSetRun(testingSet *set, const struct testingOptions *options, int *anyRun, int *anyFailed)
{
	*anyRun = 0;
	*anyFailed = 0;
	if (set == NULL)
		set = &mainTests;
	if (options == NULL)
		options = &defaultOptions;
	if (set->tests.len == 0)
		return;
	testingprivSetRun(set, options, NULL, anyFailed);
	*anyRun = 1;
}

void testingprivTLogfFull(testingT *t, const char *file, long line, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	testingprivTLogvfFull(t, file, line, format, ap);
	va_end(ap);
}

void testingprivTLogvfFull(testingT *t, const char *file, long line, const char *format, va_list ap)
{
	// TODO extract filename from file
	testingprivOutbufPrintf(t->outbuf, "%s:%ld: ", file, line);
	testingprivOutbufPrintf(t->outbuf, format, ap);
	testingprivOutbufPrintf(t->outbuf, "\n");
}

void testingTFail(testingT *t)
{
	t->failed = 1;
}

static void returnNow(testingT *t)
{
	if (!t->returned) {
		// set this now so a FailNow inside a Defer doesn't longjmp twice
		t->returned = 1;
		// run defers before calling longjmp() just to be safe
		runDefers(t);
		longjmp(t->returnNowBuf, 1);
	}
}

void testingTFailNow(testingT *t)
{
	testingTFail(t);
	returnNow(t);
}

void testingTSkipNow(testingT *t)
{
	t->skipped = 1;
	returnNow(t);
}

void testingTDefer(testingT *t, void (*f)(testingT *t, void *data), void *data)
{
	struct defer *d;

	d = testingprivNew(struct defer);
	d->f = f;
	d->data = data;
	// add to the head of the list so defers are run in reverse order of how they were added
	d->next = t->defers;
	t->defers = d;
}

void testingTRun(testingT *t, const char *subname, void (*subfunc)(testingT *t, void *data), void *data)
{
	testingT *subt;
	testingprivOutbuf *rewrittenName;
	char *fullName;

	rewrittenName = testingprivNewOutbuf();
	while (*subname != '\0') {
		const char *replaced;

		replaced = NULL;
		switch (*subname) {
		case ' ':
		case '\f':
		case '\n':
		case '\r':
		case '\t':
		case '\v':
			replaced = "_";
			break;
		case '\a':
			replaced = "\\a";
			break;
		case '\b':
			replaced = "\\b";
			break;
		}
		if (replaced != NULL)
			testingprivOutbufPrintf(rewrittenName, "%s", replaced);
		else if (isprint(*subname))
			testingprivOutbufPrintf(rewrittenName, "%c", *subname);
		else
			testingprivOutbufPrintf(rewrittenName, "\\x%x", (unsigned int) (*subname));
		subname++;
	}
	fullName = testingprivSmprintf("%s/%s", t->name, testingprivOutbufString(rewrittenName));
	testingprivOutbufFree(rewrittenName);

	subt = testingprivNew(testingT);
	initTest(subt, fullName, subfunc, data, NULL, 0);
	subt->opts = t->opts;
	if (testingprivTRun(subt, t->outbuf) != 0)
		t->failed = 1;
	testingprivFree(subt);

	testingprivFree(fullName);
}