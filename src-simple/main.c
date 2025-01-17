// Copyright 2007-2009 Russ Cox.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "regexp.h"
#include "memoize.h"
#include "vendor/cJSON.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
// Set this to 1 if you want to see regex and VM representations
#define DEBUG 0

typedef struct Query Query;

struct Query
{
	char *regex;
	char *input;
	int *rleValues;
	int rleValuesLength;
	int singleRleK;
};

struct {
	char *name;
	int (*fn)(Prog*, char*, char**, int);
} tab[] = {
	{"recursive", recursiveprog},
	{"recursiveloop", recursiveloopprog},
	{"backtrack", backtrack},
	{"thompson", thompsonvm},
	{"pike", pikevm},
};

void
usage(void)
{
	/* TODO: Diagnose cases where rle-tuned doesn't help */
	fprintf(stderr, "usage: re {none|full|indeg|loop} {none|neg|rle|rle-tuned} { regexp string | -f patternAndStr.json } { singlerlek int | multiplerlek int,int...}\n");
	fprintf(stderr, "  The first argument is the memoization strategy\n");
	fprintf(stderr, "  The second argument is the memo table encoding scheme\n");
	exit(2);
}

char *loadFile(char *fileName)
{
	FILE *f;
	long fsize;
	char *string;

	// https://stackoverflow.com/a/14002993
	f = fopen(fileName, "r");
	fseek(f, 0, SEEK_END);
	fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	string = mal(fsize + 1);
	assert(fread(string, 1, fsize, f) == fsize);
	fclose(f);

	string[fsize] = 0;
	return string;
}

Query
loadQuery(char *inFile)
{
	Query q;
	char *rawJson;
	cJSON *parsedJson, *key;

	if (access(inFile, F_OK) != 0) {
		assert(!"No such file\n");
	}

	// Read file
	logMsg(LOG_INFO, "Reading %s", inFile);
	rawJson = loadFile(inFile);
	logMsg(LOG_INFO, "Contents: <%s>", rawJson);

	// Parse contents
	logMsg(LOG_INFO, "json parse");
	parsedJson = cJSON_Parse(rawJson);
	logMsg(LOG_INFO, "%d keys", cJSON_GetArraySize(parsedJson));
	assert(cJSON_GetArraySize(parsedJson) >= 2);
	
	key = cJSON_GetObjectItem(parsedJson, "pattern");
	assert(key != NULL);
	q.regex = strdup(key->valuestring);
	logMsg(LOG_INFO, "regex: <%s>", q.regex);

	key = cJSON_GetObjectItem(parsedJson, "input");
	assert(key != NULL);
	q.input = strdup(key->valuestring);
	logMsg(LOG_INFO, "input: <%s>", q.input);
	logMsg(LOG_INFO, "length: %zu", strlen(q.input));
	// key = cJSON_GetObjectItem(parsedJson, "rleValues");
	// int array_size = cJSON_GetArraySize(key);
	// int *int_array = malloc(array_size * sizeof(int));
	// for (int i = 0; i < array_size; i++) {
    //     cJSON *item = cJSON_GetArrayItem(key, i);
	// 	int_array[i] = item->valueint;
    // }
	// q.rleValues = int_array;
	// q.rleValuesLength = array_size;
	key = cJSON_GetObjectItem(parsedJson, "rleKValue");
	q.singleRleK = key->valueint;
	cJSON_Delete(parsedJson);
	free(rawJson);
	return q;
}

int
getMemoMode(char *arg)
{
	if (strcmp(arg, "none") == 0)
		return MEMO_NONE;
	else if (strcmp(arg, "full") == 0)
		return MEMO_FULL;
	else if (strcmp(arg, "indeg") == 0)
		return MEMO_IN_DEGREE_GT1;
	else if (strcmp(arg, "loop") == 0)
		return MEMO_LOOP_DEST;
    else {
		fprintf(stderr, "Error, unknown memostrategy %s\n", arg);
		usage();
		return -1; // Compiler warning
	}
}

int
getEncoding(char *arg)
{
	if (strcmp(arg, "none") == 0)
		return ENCODING_NONE;
	else if (strcmp(arg, "neg") == 0)
		return ENCODING_NEGATIVE;
	else if (strcmp(arg, "rle") == 0)
		return ENCODING_RLE;
	else if (strcmp(arg, "rle-tuned") == 0)
		return ENCODING_RLE_TUNED;
    else {
		fprintf(stderr, "Error, unknown encoding %s\n", arg);
		usage();
		return -1; // Compiler warning
	}
}

static void
freeprog(Prog *p)
{
	int i;
	for (i = 0; i < p->len; i++) {
		Inst *inst = p->start + i;
		if (inst->edges != NULL)
			free(inst->edges);
	}
	free(p); // This also free p->start
}
char* processStringWithEscapes(const char *str) {
	char *parsedString = (char *)malloc(strlen(str) + 1);
    char *dst = parsedString; // Destination pointer for the parsed string
    while (*str) {
        if (*str == '\\') {
            str++;
            switch (*str) {
                case 'n':
                    *dst = '\n';
                    break;
                case 't':
                    *dst = '\t';
                    break;
                case '\\':
                    *dst = '\\';
                    break;
                case '\"':
                    *dst = '\"';
                    break;
                case '\'':
                    *dst = '\'';
                    break;
                default:
                    *dst = '\\';
                    dst++;
                    *dst = *str;
                    break;
            }
        } else {
            *dst = *str;
        }
        dst++;
        str++;
    }
    *dst = '\0'; // Null-terminate the parsed string

    return parsedString;
}

int
main(int argc, char **argv)
{
	int j, k, l, memoMode, memoEncoding;
	Query q;
	Regexp *re;
	Prog *prog;
	char *sub[MAXSUB]; /* Start and end pointers for each CG */

	if (argc < 4)
		usage();
	
	memoMode = getMemoMode(argv[1]);
	memoEncoding = getEncoding(argv[2]);
	if (memoMode == MEMO_NONE)
		memoEncoding = ENCODING_NONE;

	if (strcmp(argv[3], "-f") == 0) {
		q = loadQuery(argv[4]);
	} else {
		if (argc < 7)
  			usage();
		q.regex = argv[3];
		// q.input = argv[4];
		q.input = processStringWithEscapes(argv[4]);
		if (strcmp(argv[5], "singlerlek") == 0){
			q.singleRleK = strtol(argv[6], NULL, 10);
		} else {
			char *input = argv[6];
			char *token;
			int count = 0;
			int *numbers = NULL;
			char *inputCopy = strdup(input);
			if (inputCopy == NULL) {
				perror("Failed to duplicate input string");
				return 1;
			}
			token = strtok(inputCopy, ",");
			while (token != NULL) {
				count++;
				token = strtok(NULL, ",");
			}
			numbers = (int *)malloc(count * sizeof(int));
			if (numbers == NULL) {
				perror("Failed to allocate memory for numbers");
				free(inputCopy);
				return 1;
			}

			strcpy(inputCopy, input);
			token = strtok(inputCopy, ",");
			int index = 0;

			while (token != NULL) {
				char *endptr;
				errno = 0;
				long number = strtol(token, &endptr, 10);

				if (errno != 0 || *endptr != '\0' || number > INT_MAX || number < INT_MIN) {
					printf("Invalid integer: %s\n", token);
					free(numbers);
					free(inputCopy);
					return 1;
				}

				numbers[index++] = (int)number;
				token = strtok(NULL, ",");
			}
			free(inputCopy);
			q.rleValues = numbers;
			q.rleValuesLength = count;
		}
		
	}

	// Parse
	re = parse(q.regex);

	// Optimize
	if (shouldLog(LOG_DEBUG)) {
		logMsg(LOG_INFO, "Initial re:");
		printre(re);
		printf("\n");
	}
	re = transform(re);

	if (shouldLog(LOG_DEBUG)) {
		logMsg(LOG_INFO, "Transformed re:");
		printre(re);
		printf("\n");
	}

	// Compile
	prog = compile(re, memoMode, memoEncoding, q.rleValues, q.rleValuesLength, q.singleRleK);
	// if (shouldLog(LOG_DEBUG)) {
		logMsg(LOG_INFO, "Compiled :");
		printprog(prog);
		printf("\n");
	// }
	Prog_assertNoInfiniteLoops(prog);

	// Memoization settings
	prog->memoMode = memoMode;
	prog->memoEncoding = memoEncoding;
	Prog_determineMemoNodes(prog, memoMode);
	logMsg(LOG_INFO, "Will memoize %d states", prog->nMemoizedStates);

	if (shouldLog(LOG_DEBUG)) {
		logMsg(LOG_INFO, "Compiled and memo-marked:");
		printprog(prog);
		printf("\n");
	}

	// Simulate
	logMsg(LOG_INFO, "Candidate string: %s", q.input);
	for(j=0; j<nelem(tab); j++) { /* Go through all matchers */
		if (strcmp(tab[j].name, "backtrack") != 0) { /* We just care about backtrack */
			continue;
		}
		memset(sub, 0, sizeof sub);
		if(!tab[j].fn(prog, q.input, sub, nelem(sub))) {
			printf("-no match-\n");
			continue;
		}
		printf("match");
		for(k=MAXSUB; k>0; k--)
			if(sub[k-1])
				break;
		for(l=0; l<k; l+=2) {
			printf(" (");
			if(sub[l] == nil)
				printf("?");
			else
				printf("%d", (int)(sub[l] - q.input));
			printf(",");
			if(sub[l+1] == nil)
				printf("?");
			else
				printf("%d", (int)(sub[l+1] - q.input));
			printf(")");
		}
		printf("\n");
	}

	freeprog(prog);
	freereg(re);

	return 0;
}
