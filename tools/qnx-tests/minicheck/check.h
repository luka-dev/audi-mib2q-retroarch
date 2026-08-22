#ifndef RETROARCH_QNX_MINICHECK_H
#define RETROARCH_QNX_MINICHECK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*TFun)(int);
typedef struct MiniCheckSuite Suite;
typedef struct MiniCheckTCase TCase;
typedef struct MiniCheckRunner SRunner;

Suite *suite_create(const char *name);
TCase *tcase_create(const char *name);
void mini_tcase_add_test(TCase *test_case, TFun test, const char *name);
void suite_add_tcase(Suite *suite, TCase *test_case);
SRunner *srunner_create(Suite *suite);
void srunner_run_all(SRunner *runner, int print_mode);
int srunner_ntests_failed(SRunner *runner);
void srunner_free(SRunner *runner);

void mini_check_fail(const char *file, int line, const char *expression);
void mini_check_fail_int(const char *file, int line, const char *lhs_name,
      long long lhs, const char *rhs_name, long long rhs);
void mini_check_fail_uint(const char *file, int line, const char *lhs_name,
      unsigned long long lhs, const char *rhs_name, unsigned long long rhs);
void mini_check_fail_ptr(const char *file, int line, const char *lhs_name,
      const void *lhs, const char *rhs_name, const void *rhs);

#define CK_NORMAL 0
#define START_TEST(name) static void name(int _i __attribute__((unused)))
#define END_TEST
#define tcase_add_test(test_case, test) \
   mini_tcase_add_test((test_case), (test), #test)

#define ck_assert(expression) do { \
   if (!(expression)) \
      mini_check_fail(__FILE__, __LINE__, #expression); \
} while (0)

#define ck_assert_int_eq(lhs, rhs) do { \
   long long mini_lhs = (long long)(lhs); \
   long long mini_rhs = (long long)(rhs); \
   if (mini_lhs != mini_rhs) \
      mini_check_fail_int(__FILE__, __LINE__, #lhs, mini_lhs, #rhs, mini_rhs); \
} while (0)

#define ck_assert_uint_eq(lhs, rhs) do { \
   unsigned long long mini_lhs = (unsigned long long)(lhs); \
   unsigned long long mini_rhs = (unsigned long long)(rhs); \
   if (mini_lhs != mini_rhs) \
      mini_check_fail_uint(__FILE__, __LINE__, #lhs, mini_lhs, #rhs, mini_rhs); \
} while (0)

#define ck_assert_ptr_eq(lhs, rhs) do { \
   const void *mini_lhs = (const void *)(lhs); \
   const void *mini_rhs = (const void *)(rhs); \
   if (mini_lhs != mini_rhs) \
      mini_check_fail_ptr(__FILE__, __LINE__, #lhs, mini_lhs, #rhs, mini_rhs); \
} while (0)

#define ck_assert_ptr_null(value) ck_assert_ptr_eq((value), NULL)
#define ck_assert_ptr_nonnull(value) do { \
   const void *mini_value = (const void *)(value); \
   if (mini_value == NULL) \
      mini_check_fail(__FILE__, __LINE__, #value " != NULL"); \
} while (0)

#ifdef __cplusplus
}
#endif

#endif
