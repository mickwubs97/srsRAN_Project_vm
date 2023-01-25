/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "e1ap_cu_up_test_helpers.h"

using namespace srsgnb;
using namespace srs_cu_up;

e1ap_cu_up_test::e1ap_cu_up_test()
{
  test_logger.set_level(srslog::basic_levels::debug);
  e1ap_logger.set_level(srslog::basic_levels::debug);
  srslog::init();

  e1ap = create_e1(msg_notifier, cu_up_notifier);
}

e1ap_cu_up_test::~e1ap_cu_up_test()
{
  // flush logger after each test
  srslog::flush();
}
