#include "check.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

struct MiniCheckTest {
   TFun function;
   const char *name;
};

struct MiniCheckTCase {
   const char *name;
   struct MiniCheckTest tests[256];
   size_t count;
};

struct MiniCheckSuite {
   const char *name;
   TCase *test_case;
};

struct MiniCheckRunner {
   Suite *suite;
   int failed;
   int executed;
};

static jmp_buf mini_check_jump;
static int mini_check_can_jump;

static void *checked_alloc(size_t size) {
   void *memory = calloc(1, size);
   if (!memory) {
      fprintf(stderr, "MINICHECK internal allocation failure\n");
      exit(125);
   }
   return memory;
}

Suite *suite_create(const char *name) {
   Suite *suite = (Suite *)checked_alloc(sizeof(*suite));
   suite->name = name;
   return suite;
}

TCase *tcase_create(const char *name) {
   TCase *test_case = (TCase *)checked_alloc(sizeof(*test_case));
   test_case->name = name;
   return test_case;
}

void mini_tcase_add_test(TCase *test_case, TFun test, const char *name) {
   if (test_case->count >= sizeof(test_case->tests) / sizeof(test_case->tests[0])) {
      fprintf(stderr, "MINICHECK too many tests in %s\n", test_case->name);
      exit(125);
   }
   test_case->tests[test_case->count].function = test;
   test_case->tests[test_case->count].name = name;
   ++test_case->count;
}

void suite_add_tcase(Suite *suite, TCase *test_case) {
   suite->test_case = test_case;
}

SRunner *srunner_create(Suite *suite) {
   SRunner *runner = (SRunner *)checked_alloc(sizeof(*runner));
   runner->suite = suite;
   return runner;
}

static void report_failure_prefix(const char *file, int line) {
   fprintf(stderr, "MINICHECK FAIL %s:%d: ", file, line);
}

static void finish_failure(void) {
   fputc('\n', stderr);
   fflush(stderr);
   if (mini_check_can_jump)
      longjmp(mini_check_jump, 1);
   exit(125);
}

void mini_check_fail(const char *file, int line, const char *expression) {
   report_failure_prefix(file, line);
   fprintf(stderr, "%s", expression);
   finish_failure();
}

void mini_check_fail_int(const char *file, int line, const char *lhs_name,
      long long lhs, const char *rhs_name, long long rhs) {
   report_failure_prefix(file, line);
   fprintf(stderr, "%s=%lld != %s=%lld", lhs_name, lhs, rhs_name, rhs);
   finish_failure();
}

void mini_check_fail_uint(const char *file, int line, const char *lhs_name,
      unsigned long long lhs, const char *rhs_name, unsigned long long rhs) {
   report_failure_prefix(file, line);
   fprintf(stderr, "%s=%llu != %s=%llu", lhs_name, lhs, rhs_name, rhs);
   finish_failure();
}

void mini_check_fail_ptr(const char *file, int line, const char *lhs_name,
      const void *lhs, const char *rhs_name, const void *rhs) {
   report_failure_prefix(file, line);
   fprintf(stderr, "%s=%p != %s=%p", lhs_name, lhs, rhs_name, rhs);
   finish_failure();
}

void srunner_run_all(SRunner *runner, int print_mode) {
   TCase *test_case = runner->suite->test_case;
   size_t index;
   (void)print_mode;

   printf("MINICHECK suite=%s case=%s tests=%u\n", runner->suite->name,
         test_case->name, (unsigned)test_case->count);
   for (index = 0; index < test_case->count; ++index) {
      ++runner->executed;
      mini_check_can_jump = 1;
      printf("RUN  %s\n", test_case->tests[index].name);
      fflush(stdout);
      if (setjmp(mini_check_jump) == 0) {
         test_case->tests[index].function(0);
         printf("PASS %s\n", test_case->tests[index].name);
      } else {
         ++runner->failed;
         printf("FAIL %s\n", test_case->tests[index].name);
      }
      mini_check_can_jump = 0;
   }
   printf("MINICHECK result=%s executed=%d failed=%d\n",
         runner->failed ? "FAIL" : "PASS", runner->executed, runner->failed);
   fflush(stdout);
}

int srunner_ntests_failed(SRunner *runner) {
   return runner->failed;
}

void srunner_free(SRunner *runner) {
   if (runner) {
      if (runner->suite) {
         free(runner->suite->test_case);
         free(runner->suite);
      }
      free(runner);
   }
}
