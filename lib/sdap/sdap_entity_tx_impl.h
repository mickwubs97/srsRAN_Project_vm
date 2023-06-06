/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "sdap_session_logger.h"
#include "srsran/sdap/sdap.h"
#include "srsran/support/timers.h"

namespace srsran {

namespace srs_cu_up {

class sdap_entity_tx_impl
{
public:
  sdap_entity_tx_impl(uint32_t              ue_index,
                      pdu_session_id_t      sid,
                      qos_flow_id_t         qfi_,
                      drb_id_t              drb_id_,
                      unique_timer&         ue_inactivity_timer_,
                      sdap_tx_pdu_notifier& pdu_notifier_) :
    logger("SDAP", {ue_index, sid, "DL"}),
    qfi(qfi_),
    drb_id(drb_id_),
    ue_inactivity_timer(ue_inactivity_timer_),
    pdu_notifier(pdu_notifier_)
  {
  }

  void handle_sdu(byte_buffer sdu)
  {
    // pass through
    logger.log_debug("TX PDU. qfi={} pdu_len={}", qfi, sdu.length());
    pdu_notifier.on_new_pdu(std::move(sdu));
    ue_inactivity_timer.run();
  }

  drb_id_t get_drb_id() const { return drb_id; }

private:
  sdap_session_logger   logger;
  qos_flow_id_t         qfi;
  drb_id_t              drb_id;
  unique_timer&         ue_inactivity_timer;
  sdap_tx_pdu_notifier& pdu_notifier;
};

} // namespace srs_cu_up

} // namespace srsran
