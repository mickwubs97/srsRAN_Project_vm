/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "pusch_processor_impl.h"
#include "pusch_decoder_buffer_dummy.h"
#include "pusch_processor_notifier_adaptor.h"
#include "srsran/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "srsran/phy/upper/channel_processors/pusch/pusch_decoder_buffer.h"
#include "srsran/phy/upper/unique_rx_softbuffer.h"
#include "srsran/ran/pusch/ulsch_info.h"
#include "srsran/ran/sch_dmrs_power.h"

using namespace srsran;

// Dummy PUSCH decoder buffer. Used for PUSCH transmissions without SCH data.
static pusch_decoder_buffer_dummy decoder_buffer_dummy;

bool pusch_processor_validator_impl::is_valid(const pusch_processor::pdu_t& pdu) const
{
  unsigned nof_symbols_slot = get_nsymb_per_slot(pdu.cp);

  // The BWP size exceeds the grid size.
  if ((pdu.bwp_start_rb + pdu.bwp_size_rb) > ce_dims.nof_prb) {
    return false;
  }

  // The implementation only works with a single transmit layer.
  if (pdu.nof_tx_layers > ce_dims.nof_tx_layers) {
    return false;
  }

  // The number of receive ports cannot exceed the maximum dimensions.
  if (pdu.rx_ports.size() > ce_dims.nof_rx_ports) {
    return false;
  }

  // The frequency allocation is not compatible with the BWP parameters.
  if (!pdu.freq_alloc.is_bwp_valid(pdu.bwp_start_rb, pdu.bwp_size_rb)) {
    return false;
  }

  // Currently, none of the UCI field sizes can exceed 11 bit.
  static constexpr unsigned max_uci_len = 11;
  if ((pdu.uci.nof_harq_ack > max_uci_len) || (pdu.uci.nof_csi_part1 > max_uci_len)) {
    return false;
  }

  // CSI Part 2 multiplexing is not supported.
  if (pdu.uci.nof_csi_part2 != 0) {
    return false;
  }

  // The number of symbols carrying DM-RS must be greater than zero.
  if (pdu.dmrs_symbol_mask.size() != nof_symbols_slot) {
    return false;
  }

  // The number of symbols carrying DM-RS must be greater than zero.
  if (pdu.dmrs_symbol_mask.none()) {
    return false;
  }

  // The index of the first OFDM symbol carrying DM-RS shall be equal to or greater than the first symbol allocated to
  // transmission.
  int first_dmrs_symbol_index = pdu.dmrs_symbol_mask.find_lowest(true);
  if (static_cast<unsigned>(first_dmrs_symbol_index) < pdu.start_symbol_index) {
    return false;
  }

  // The index of the last OFDM symbol carrying DM-RS shall not be larger than the last symbol allocated to
  // transmission.
  int last_dmrs_symbol_index = pdu.dmrs_symbol_mask.find_highest(true);
  if (static_cast<unsigned>(last_dmrs_symbol_index) >= (pdu.start_symbol_index + pdu.nof_symbols)) {
    return false;
  }

  // None of the occupied symbols must exceed the slot size.
  if (nof_symbols_slot < (pdu.start_symbol_index + pdu.nof_symbols)) {
    return false;
  }

  // Only DM-RS Type 1 is supported.
  if (pdu.dmrs != dmrs_type::TYPE1) {
    return false;
  }

  // Only two CDM groups without data is supported.
  if (pdu.nof_cdm_groups_without_data != 2) {
    return false;
  }

  // DC position is outside the channel estimate dimensions.
  interval<unsigned> dc_position_range(0, ce_dims.nof_prb * NRE);
  if (pdu.dc_position.has_value() && !dc_position_range.contains(pdu.dc_position.value())) {
    return false;
  }

  return true;
}

pusch_processor_impl::pusch_processor_impl(pusch_processor_configuration& config) :
  estimator(std::move(config.estimator)),
  demodulator(std::move(config.demodulator)),
  demultiplex(std::move(config.demultiplex)),
  decoder(std::move(config.decoder)),
  uci_dec(std::move(config.uci_dec)),
  harq_ack_decoder(*uci_dec, pusch_constants::CODEWORD_MAX_SIZE.value()),
  csi_part1_decoder(*uci_dec, pusch_constants::CODEWORD_MAX_SIZE.value()),
  csi_part2_decoder(*uci_dec, pusch_constants::CODEWORD_MAX_SIZE.value()),
  ch_estimate(config.ce_dims),
  dec_nof_iterations(config.dec_nof_iterations),
  dec_enable_early_stop(config.dec_enable_early_stop),
  csi_sinr_calc_method(config.csi_sinr_calc_method)
{
  srsran_assert(estimator, "Invalid estimator.");
  srsran_assert(demodulator, "Invalid demodulator.");
  srsran_assert(demultiplex, "Invalid demultiplex.");
  srsran_assert(decoder, "Invalid decoder.");
  srsran_assert(uci_dec, "Invalid UCI decoder.");
  srsran_assert(dec_nof_iterations != 0, "The decoder number of iterations must be non-zero.");
}

void pusch_processor_impl::process(span<uint8_t>                    data,
                                   rx_softbuffer&                   softbuffer,
                                   pusch_processor_result_notifier& notifier,
                                   const resource_grid_reader&      grid,
                                   const pusch_processor::pdu_t&    pdu)
{
  assert_pdu(pdu);

  // Number of RB used by this transmission.
  unsigned nof_rb = pdu.freq_alloc.get_nof_rb();

  // Get RB mask relative to Point A. It assumes PUSCH is never interleaved.
  bounded_bitset<MAX_RB> rb_mask = pdu.freq_alloc.get_prb_mask(pdu.bwp_start_rb, pdu.bwp_size_rb);

  // Get UL-SCH information.
  ulsch_configuration ulsch_config;
  ulsch_config.tbs                   = units::bytes(data.size()).to_bits();
  ulsch_config.mcs_descr             = pdu.mcs_descr;
  ulsch_config.nof_harq_ack_bits     = units::bits(pdu.uci.nof_harq_ack);
  ulsch_config.nof_csi_part1_bits    = units::bits(pdu.uci.nof_csi_part1);
  ulsch_config.nof_csi_part2_bits    = units::bits(pdu.uci.nof_csi_part2);
  ulsch_config.alpha_scaling         = pdu.uci.alpha_scaling;
  ulsch_config.beta_offset_harq_ack  = pdu.uci.beta_offset_harq_ack;
  ulsch_config.beta_offset_csi_part1 = pdu.uci.beta_offset_csi_part1;
  ulsch_config.beta_offset_csi_part2 = pdu.uci.beta_offset_csi_part2;
  ulsch_config.nof_rb                = nof_rb;
  ulsch_config.start_symbol_index    = pdu.start_symbol_index;
  ulsch_config.nof_symbols           = pdu.nof_symbols;
  ulsch_config.dmrs_type             = pdu.dmrs == dmrs_type::TYPE1 ? dmrs_config_type::type1 : dmrs_config_type::type2;
  ulsch_config.dmrs_symbol_mask      = pdu.dmrs_symbol_mask;
  ulsch_config.nof_cdm_groups_without_data = pdu.nof_cdm_groups_without_data;
  ulsch_config.nof_layers                  = pdu.nof_tx_layers;
  ulsch_information info                   = get_ulsch_information(ulsch_config);

  // Estimate channel.
  dmrs_pusch_estimator::configuration ch_est_config;
  ch_est_config.slot          = pdu.slot;
  ch_est_config.type          = pdu.dmrs;
  ch_est_config.scrambling_id = pdu.scrambling_id;
  ch_est_config.n_scid        = pdu.n_scid;
  ch_est_config.scaling       = convert_dB_to_amplitude(-get_sch_to_dmrs_ratio_dB(pdu.nof_cdm_groups_without_data));
  ch_est_config.c_prefix      = pdu.cp;
  ch_est_config.symbols_mask  = pdu.dmrs_symbol_mask;
  ch_est_config.rb_mask       = rb_mask;
  ch_est_config.first_symbol  = pdu.start_symbol_index;
  ch_est_config.nof_symbols   = pdu.nof_symbols;
  ch_est_config.nof_tx_layers = pdu.nof_tx_layers;
  ch_est_config.rx_ports.assign(pdu.rx_ports.begin(), pdu.rx_ports.end());
  estimator->estimate(ch_estimate, grid, ch_est_config);

  // Handles the direct current if it is present.
  if (pdu.dc_position.has_value()) {
    unsigned dc_position = pdu.dc_position.value();
    for (unsigned i_port = 0, i_port_end = pdu.rx_ports.size(); i_port != i_port_end; ++i_port) {
      for (unsigned i_layer = 0, i_layer_end = pdu.nof_tx_layers; i_layer != i_layer_end; ++i_layer) {
        for (unsigned i_symbol = pdu.start_symbol_index, i_symbol_end = pdu.start_symbol_index + pdu.nof_symbols;
             i_symbol != i_symbol_end;
             ++i_symbol) {
          // Extract channel estimates for the OFDM symbol, port and layer.
          span<cf_t> ce = ch_estimate.get_symbol_ch_estimate(i_symbol, i_port, i_layer);

          // Set DC to zero.
          ce[dc_position] = 0;
        }
      }
    }
  }

  // Extract channel state information.
  channel_state_information csi(csi_sinr_calc_method);
  ch_estimate.get_channel_state_information(csi);

  // Prepare demultiplex configuration.
  ulsch_demultiplex::configuration demux_config;
  demux_config.modulation                  = pdu.mcs_descr.modulation;
  demux_config.nof_layers                  = pdu.nof_tx_layers;
  demux_config.nof_prb                     = nof_rb;
  demux_config.start_symbol_index          = pdu.start_symbol_index;
  demux_config.nof_symbols                 = pdu.nof_symbols;
  demux_config.nof_harq_ack_rvd            = info.nof_harq_ack_rvd.value();
  demux_config.dmrs                        = pdu.dmrs;
  demux_config.dmrs_symbol_mask            = ulsch_config.dmrs_symbol_mask;
  demux_config.nof_cdm_groups_without_data = ulsch_config.nof_cdm_groups_without_data;
  demux_config.nof_harq_ack_bits           = ulsch_config.nof_harq_ack_bits.value();
  demux_config.nof_enc_harq_ack_bits       = info.nof_harq_ack_bits.value();
  demux_config.nof_csi_part1_bits          = ulsch_config.nof_csi_part1_bits.value();
  demux_config.nof_enc_csi_part1_bits      = info.nof_csi_part1_bits.value();

  // Convert DM-RS symbol mask to array.
  std::array<bool, MAX_NSYMB_PER_SLOT> dmrs_symbol_mask = {};
  pdu.dmrs_symbol_mask.for_each(
      0, pdu.dmrs_symbol_mask.size(), [&dmrs_symbol_mask](unsigned i_symb) { dmrs_symbol_mask[i_symb] = true; });

  bool has_sch_data = pdu.codeword.has_value();

  // Prepare decoder buffers with dummy instances.
  std::reference_wrapper<pusch_decoder_buffer> decoder_buffer(decoder_buffer_dummy);
  std::reference_wrapper<pusch_decoder_buffer> harq_ack_buffer(decoder_buffer_dummy);
  std::reference_wrapper<pusch_decoder_buffer> csi_part1_buffer(decoder_buffer_dummy);

  // Prepare notifiers.
  pusch_processor_notifier_adaptor notifier_adaptor(notifier, csi);

  if (has_sch_data) {
    // Prepare decoder configuration.
    pusch_decoder::configuration decoder_config;
    decoder_config.base_graph          = pdu.codeword.value().ldpc_base_graph;
    decoder_config.rv                  = pdu.codeword.value().rv;
    decoder_config.mod                 = pdu.mcs_descr.modulation;
    decoder_config.Nref                = pdu.tbs_lbrm_bytes * 8;
    decoder_config.nof_layers          = pdu.nof_tx_layers;
    decoder_config.nof_ldpc_iterations = dec_nof_iterations;
    decoder_config.use_early_stop      = dec_enable_early_stop;
    decoder_config.new_data            = pdu.codeword.value().new_data;

    // Setup decoder.
    decoder_buffer = decoder->new_data(data, softbuffer, notifier_adaptor.get_sch_data_notifier(), decoder_config);
  }

  // Prepares HARQ-ACK notifier and buffer.
  if (pdu.uci.nof_harq_ack != 0) {
    harq_ack_buffer = harq_ack_decoder.new_transmission(
        pdu.uci.nof_harq_ack, pdu.mcs_descr.modulation, notifier_adaptor.get_harq_ack_notifier());
  }

  // Prepares CSI Part 1 notifier and buffer.
  if (pdu.uci.nof_csi_part1 != 0) {
    csi_part1_buffer = csi_part1_decoder.new_transmission(
        pdu.uci.nof_csi_part1, pdu.mcs_descr.modulation, notifier_adaptor.get_csi_part1_notifier());
  }

  // Demultiplex SCH data, HARQ-ACK and CSI Part 1.
  pusch_codeword_buffer& demodulator_buffer =
      demultiplex->demultiplex(decoder_buffer, harq_ack_buffer, csi_part1_buffer, demux_config);

  // Demodulate.
  pusch_demodulator::configuration demod_config;
  demod_config.rnti                        = pdu.rnti;
  demod_config.rb_mask                     = rb_mask;
  demod_config.modulation                  = pdu.mcs_descr.modulation;
  demod_config.start_symbol_index          = pdu.start_symbol_index;
  demod_config.nof_symbols                 = pdu.nof_symbols;
  demod_config.dmrs_symb_pos               = dmrs_symbol_mask;
  demod_config.dmrs_config_type            = pdu.dmrs;
  demod_config.nof_cdm_groups_without_data = pdu.nof_cdm_groups_without_data;
  demod_config.n_id                        = pdu.n_id;
  demod_config.nof_tx_layers               = pdu.nof_tx_layers;
  demod_config.rx_ports                    = pdu.rx_ports;
  demodulator->demodulate(
      demodulator_buffer, notifier_adaptor.get_demodulator_notifier(), grid, ch_estimate, demod_config);
}

void pusch_processor_impl::assert_pdu(const pusch_processor::pdu_t& pdu) const
{
  // Make sure the configuration is supported.
  srsran_assert((pdu.bwp_start_rb + pdu.bwp_size_rb) <= ch_estimate.size().nof_prb,
                "The sum of the BWP start (i.e., {}) and size (i.e., {}) exceeds the maximum grid size (i.e., {} PRB).",
                pdu.bwp_start_rb,
                pdu.bwp_size_rb,
                ch_estimate.size().nof_prb);
  srsran_assert(pdu.dmrs == dmrs_type::TYPE1, "Only DM-RS Type 1 is currently supported.");
  srsran_assert(pdu.nof_cdm_groups_without_data == 2, "Only two CDM groups without data are currently supported.");
  srsran_assert(
      pdu.nof_tx_layers <= ch_estimate.size().nof_tx_layers,
      "The number of transmit layers (i.e., {}) exceeds the maximum number of transmission layers (i.e., {}).",
      pdu.nof_tx_layers,
      ch_estimate.size().nof_tx_layers);
  srsran_assert(pdu.rx_ports.size() <= ch_estimate.size().nof_rx_ports,
                "The number of receive ports (i.e., {}) exceeds the maximum number of receive ports (i.e., {}).",
                pdu.rx_ports.size(),
                ch_estimate.size().nof_rx_ports);

  static constexpr unsigned max_uci_field_len = 11;
  srsran_assert(pdu.uci.nof_harq_ack <= max_uci_field_len,
                "HARQ-ACK UCI field length (i.e., {}) exceeds the maximum supported length (i.e., {})",
                pdu.uci.nof_harq_ack,
                max_uci_field_len);

  srsran_assert(pdu.uci.nof_csi_part1 <= max_uci_field_len,
                "CSI Part 1 UCI field length (i.e., {}) exceeds the maximum supported length (i.e., {})",
                pdu.uci.nof_csi_part1,
                max_uci_field_len);

  srsran_assert((pdu.uci.nof_csi_part2 == 0), "CSI Part 2 is not currently implemented.");

  // Check DC is whithin the CE.
  if (pdu.dc_position.has_value()) {
    interval<unsigned> dc_position_range(0, ch_estimate.size().nof_prb * NRE);
    srsran_assert(dc_position_range.contains(pdu.dc_position.value()),
                  "DC position (i.e., {}) is out of range {}.",
                  pdu.dc_position.value(),
                  dc_position_range);
  }
}
