/* unit test the sproxyd protocol backend */
#include <sys/types.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <check.h>
#include <droplet.h>

#include "toyctl.h"
#include "testutils.h"
#include "utest_main.h"

static struct server_state *state = NULL;
static dpl_dict_t *profile = NULL;
static dpl_ctx_t *ctx = NULL;

static void
setup(void)
{
  int r;

  unsetenv("DPLDIR");
  unsetenv("DPLPROFILE");
  dpl_init();

  r = toyserver_start(NULL, &state);
  dpl_assert_int_eq(r, 0);

  profile = dpl_dict_new(13);
  dpl_assert_ptr_not_null(profile);
  dpl_assert_int_eq(DPL_SUCCESS, dpl_dict_add(profile, "host", toyserver_addrlist(state), 0));
  dpl_assert_int_eq(DPL_SUCCESS, dpl_dict_add(profile, "backend", "sproxyd", 0));
  dpl_assert_int_eq(DPL_SUCCESS, dpl_dict_add(profile, "base_path", "/proxy/chord", 0));
  dpl_assert_int_eq(DPL_SUCCESS, dpl_dict_add(profile, "droplet_dir", "/never/seen", 0));
  dpl_assert_int_eq(DPL_SUCCESS, dpl_dict_add(profile, "profile_name", "viral", 0));
  /* need this to disable the event log, otherwise the droplet_dir needs to exist */
  dpl_assert_int_eq(DPL_SUCCESS, dpl_dict_add(profile, "pricing_dir", "", 0));
}

static void
teardown(void)
{
  if (ctx)
    dpl_ctx_free(ctx);
  dpl_dict_free(profile);
  toyserver_stop(state);
  dpl_free();
}

START_TEST(get_set_test)
{
  dpl_status_t s;
  char *expected_uri;
  int r;
  char id[41];
  static const char data[] =
    "Carles wolf yr Austin, chambray twee lo-fi iPhone brunch Neutra"
    "slow-carb. Viral +1 kitsch fashion axe wolf.  Selvage flexitarian"
    "ugh banjo Godard, jean shorts occupy Marfa fingerstache literally"
    "whatever keffiyeh put a bird on it biodiesel brunch. Forage plaid"
    "wolf kitsch Etsy. Literally ugh Carles, Intelligentsia sartorial";

  /* create a context with all defaults */
  ctx = dpl_ctx_new_from_dict(profile);
  dpl_assert_ptr_not_null(ctx);

  s = dpl_gen_random_key(ctx, DPL_STORAGE_CLASS_STANDARD, /*custom*/NULL, id, sizeof(id));
  dpl_assert_int_eq(DPL_SUCCESS, s);

  s = dpl_put_id(ctx,
		"foobucket",
		id,
		/*options*/NULL,
		DPL_FTYPE_REG,
		/*condition*/NULL,
		/*range*/NULL,
		/*metadata*/NULL,
		/*sysmd*/NULL,
		data, sizeof(data)-1);
  dpl_assert_int_eq(DPL_SUCCESS, s);

  dpl_assert_str_eq(state->request.host, toyserver_addrlist(state));
  expected_uri = strconcat("/proxy/chord/", id, (char *)NULL);
  dpl_assert_str_eq(state->request.uri, expected_uri);
  dpl_assert_str_eq(state->request.body, data);
  dpl_assert_int_eq(state->reply.status, 200);

//   s = dpl_get_id(ctx, "foobucket", id, /*options*/NULL, DPL_FTYPE_REG,
//   /*condition*/NULL, /*range*/NULL, char **data_bufp, unsigned int *data_lenp, dpl_dict_t **metadatap, dpl_sysmd_t *sysmdp);
//   dpl_assert_int_eq(DPL_SUCCESS, s);

  free(expected_uri);
}

END_TEST
Suite *
sproxyd_suite()
{
  Suite *s = suite_create("sproxyd");
  TCase *t = tcase_create("base");
  tcase_add_checked_fixture(t, setup, teardown);
  tcase_add_test(t, get_set_test);
  suite_add_tcase(s, t);
  return s;
}

