/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "lib/pcap/rlc_pcap_impl.h"
#include "lib/rlc/rlc_am_pdu.h"
#include <gtest/gtest.h>
#include <list>

using namespace srsran;

void write_pcap_nr_thread_function_byte_buffer(srsran::rlc_pcap* pcap, uint32_t num_pdus);
void write_pcap_nr_thread_function_large_byte_buffer(srsran::rlc_pcap* pcap, uint32_t num_pdus);
void write_pcap_nr_thread_function_spans(srsran::rlc_pcap* pcap, uint32_t num_pdus);

class pcap_rlc_test : public ::testing::Test
{
protected:
  void SetUp() override
  {
    srslog::fetch_basic_logger("PCAP").set_level(srslog::basic_levels::debug);
    srslog::fetch_basic_logger("PCAP").set_hex_dump_max_size(-1);

    logger.set_level(srslog::basic_levels::debug);
    logger.set_hex_dump_max_size(-1);

    // Start the log backend.
    srslog::init();

    config.sn_field_length = rlc_am_sn_size::size18bits;
  }

  void TearDown() override
  {
    logger.info("Closing PCAP handle");
    rlc_pcap_writer.close();
    // flush logger after each test
    srslog::flush();
  }

  /// \brief Creates a byte_buffer serving as SDU for RLC
  ///
  /// The produced SDU contains an incremental sequence of bytes starting with the value given by first_byte,
  /// i.e. if first_byte = 0xfc, the SDU will be 0xfc 0xfe 0xff 0x00 0x01 ...
  /// \param sdu_size Size of the SDU
  /// \param first_byte Value of the first byte
  /// \return the produced SDU as a byte_buffer
  byte_buffer create_sdu(uint32_t sdu_size, uint8_t first_byte = 0) const
  {
    byte_buffer sdu_buf;
    for (uint32_t k = 0; k < sdu_size; ++k) {
      sdu_buf.append(first_byte + k);
    }
    return sdu_buf;
  }

  /// \brief Creates a list of RLC AMD PDU(s) containing either one RLC SDU or multiple RLC SDU segments
  ///
  /// The associated SDU contains an incremental sequence of bytes starting with the value given by first_byte,
  /// i.e. if first_byte = 0xfc and no segmentation, the PDU will be <header> 0xfc 0xfe 0xff 0x00 0x01 ...
  ///
  /// Note: Segmentation is applied if segment_size < sdu_size; Otherwise only one PDU with full SDU is produced,
  /// and the resulting list will hold only one PDU
  ///
  /// \param[out] pdu_list Reference to a list<byte_buffer> that is filled with the produced PDU(s)
  /// \param[out] sdu Reference to a byte_buffer that is filled with the associated SDU
  /// \param[in] sn The sequence number to be put in the PDU header
  /// \param[in] sdu_size Size of the SDU
  /// \param[in] segment_size Maximum payload size of each SDU or SDU segment.
  /// \param[in] first_byte Value of the first SDU payload byte
  void create_pdus(std::list<byte_buffer>& pdu_list,
                   byte_buffer&            sdu,
                   uint32_t                sn,
                   uint32_t                sdu_size,
                   uint32_t                segment_size,
                   uint8_t                 first_byte = 0) const
  {
    ASSERT_GT(sdu_size, 0) << "Invalid argument: Cannot create PDUs with zero-sized SDU";
    ASSERT_GT(segment_size, 0) << "Invalid argument: Cannot create PDUs with zero-sized SDU segments";

    sdu = create_sdu(sdu_size, first_byte);
    pdu_list.clear();
    byte_buffer_view rest = {sdu};

    rlc_am_pdu_header hdr = {};
    hdr.dc                = rlc_dc_field::data;
    hdr.p                 = 0;
    hdr.si                = rlc_si_field::full_sdu;
    hdr.sn_size           = config.sn_field_length;
    hdr.sn                = sn;
    hdr.so                = 0;
    do {
      byte_buffer_view payload = {};
      if (rest.length() > segment_size) {
        // first or middle segment
        if (hdr.so == 0) {
          hdr.si = rlc_si_field::first_segment;
        } else {
          hdr.si = rlc_si_field::middle_segment;
        }

        // split into payload and rest
        std::pair<byte_buffer_view, byte_buffer_view> split = rest.split(segment_size);

        payload = std::move(split.first);
        rest    = std::move(split.second);
      } else {
        // last segment or full PDU
        if (hdr.so == 0) {
          hdr.si = rlc_si_field::full_sdu;
        } else {
          hdr.si = rlc_si_field::last_segment;
        }

        // full payload, no rest
        payload = std::move(rest);
        rest    = {};
      }
      byte_buffer pdu_buf;
      logger.debug("AMD PDU header: {}", hdr);
      ASSERT_TRUE(rlc_am_write_data_pdu_header(hdr, pdu_buf));
      pdu_buf.append(payload);
      pdu_list.push_back(std::move(pdu_buf));

      // update segment offset for next iteration
      hdr.so += payload.length();
    } while (rest.length() > 0);
  }

  srslog::basic_logger& logger = srslog::fetch_basic_logger("TEST");
  rlc_rx_am_config      config;
  srsran::rlc_pcap_impl rlc_pcap_writer;
};

TEST_F(pcap_rlc_test, write_rlc_am_pdu)
{
  rlc_pcap_writer.open("/tmp/write_rlc_am_pdu.pcap");

  uint32_t sn_state = 0;
  uint32_t sdu_size = 1;

  std::list<byte_buffer> pdu_list = {};
  byte_buffer            sdu;
  ASSERT_NO_FATAL_FAILURE(create_pdus(pdu_list, sdu, sn_state, sdu_size, sdu_size, sn_state));

  rlc_tx_am_config tx_cfg;
  tx_cfg.sn_field_length              = config.sn_field_length;
  srsran::rlc_nr_context_info context = {du_ue_index_t::MIN_DU_UE_INDEX, srb_id_t::srb1, tx_cfg};

  rlc_pcap_writer.push_pdu(context, pdu_list.front().deep_copy());
}
