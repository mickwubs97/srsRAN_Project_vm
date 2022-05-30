#ifndef SRSGNB_F1_INTERFACE_F1U_DU_H
#define SRSGNB_F1_INTERFACE_F1U_DU_H

#include "srsgnb/ran/nr_up_protocol.h"

namespace srsgnb {

/// This interface represents a single F1-U bearer in the DU used to transmit NR user plane messages.
class f1u_du_message_handler
{
public:
  virtual ~f1u_du_message_handler() = default;

  /// Handle the incoming NR user plane message.
  virtual void handle_message(nr_up_ul_message msg) = 0;
};

/// This interface notifies the reception of new messages over a single F1-U bearer in the DU.
class f1u_du_message_notifier
{
public:
  virtual ~f1u_du_message_notifier() = default;

  /// This callback is invoked on each received NR user plane message.
  virtual void on_new_message(nr_up_dl_message msg) = 0;
};

} // namespace srsgnb

#endif // SRSGNB_F1_INTERFACE_F1U_DU_H
