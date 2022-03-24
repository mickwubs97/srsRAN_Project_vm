
#include "srsgnb/adt/byte_buffer.h"
#include "srsgnb/support/test_utils.h"
#include <list>

using namespace srsgnb;

static_assert(std::is_same<byte_buffer::value_type, uint8_t>::value, "Invalid valid_type");
static_assert(std::is_same<byte_buffer::iterator::value_type, uint8_t>::value, "Invalid valid_type");
static_assert(std::is_same<byte_buffer::const_iterator::value_type, uint8_t>::value, "Invalid valid_type");
static_assert(std::is_same<byte_buffer::iterator::reference, uint8_t&>::value, "Invalid reference type");
static_assert(std::is_same<byte_buffer::const_iterator::reference, const uint8_t&>::value, "Invalid reference type");
static_assert(std::is_same<byte_buffer::const_iterator::pointer, const uint8_t*>::value, "Invalid pointer type");

void test_byte_buffer_append()
{
  byte_buffer pdu;
  TESTASSERT(pdu.empty());
  TESTASSERT_EQ(0, pdu.length());

  std::vector<uint8_t> bytes = {1, 2, 3, 4, 5, 6};

  pdu.append(bytes);
  TESTASSERT_EQ(pdu.length(), bytes.size());
}

void test_byte_buffer_compare()
{
  byte_buffer          pdu, pdu2, pdu3, pdu4;
  std::vector<uint8_t> bytes  = {1, 2, 3, 4, 5, 6};
  std::vector<uint8_t> bytes2 = {1, 2, 3, 4, 5};
  std::vector<uint8_t> bytes3 = {2, 2, 3, 4, 5, 6};

  pdu.append(bytes);

  TESTASSERT(pdu == bytes);
  TESTASSERT(bytes == pdu);
  TESTASSERT(bytes2 != pdu);
  TESTASSERT(bytes3 != pdu);

  pdu2.append(bytes);
  pdu3.append(bytes2);
  pdu4.append(bytes3);

  TESTASSERT(pdu == pdu2);
  TESTASSERT(pdu != pdu3);
  TESTASSERT(pdu != pdu4);
  TESTASSERT(pdu2 != pdu4);
  TESTASSERT(pdu3 != pdu4);
}

void test_byte_buffer_iterator()
{
  byte_buffer pdu;

  std::vector<uint8_t> bytes = {1, 2, 3, 4, 5, 6};
  pdu.append(bytes);

  size_t i = 0;
  for (byte_buffer::iterator it = pdu.begin(); it != pdu.end(); ++it, ++i) {
    TESTASSERT_EQ(bytes[i], *it);
  }
  TESTASSERT_EQ(bytes.size(), i);

  i = 0;
  for (byte_buffer::const_iterator it = pdu.cbegin(); it != pdu.cend(); ++it, ++i) {
    TESTASSERT_EQ(bytes[i], *it);
  }
  TESTASSERT_EQ(bytes.size(), i);
}

void test_byte_buffer_clone()
{
  byte_buffer pdu;

  std::vector<uint8_t> bytes = {1, 2, 3, 4, 5, 6};
  pdu.append(bytes);

  byte_buffer pdu2;
  pdu2 = pdu.clone();
  TESTASSERT(not pdu2.empty() and not pdu.empty());
  TESTASSERT_EQ(pdu.length(), pdu2.length());
  TESTASSERT(pdu == pdu2);
  TESTASSERT(pdu2 == bytes);

  pdu2.append(bytes);
  TESTASSERT(pdu != pdu2);
  TESTASSERT(pdu2 != bytes);
  TESTASSERT_EQ(pdu.length() * 2, pdu2.length());
}

void test_byte_buffer_move()
{
  byte_buffer          pdu;
  std::vector<uint8_t> bytes = {1, 2, 3, 4, 5, 6};
  pdu.append(bytes);

  byte_buffer pdu2{std::move(pdu)};
  TESTASSERT(not pdu2.empty() and pdu.empty());
  TESTASSERT(pdu == bytes);
}

void test_byte_buffer_formatter()
{
  byte_buffer          pdu;
  std::vector<uint8_t> bytes = {1, 2, 3, 4, 15, 16, 255};
  pdu.append(bytes);

  fmt::print("PDU: {}\n", pdu);
  std::string result = fmt::format("{}", pdu);
  TESTASSERT(result == "01 02 03 04 0f 10 ff");
}

int main()
{
  test_byte_buffer_append();
  test_byte_buffer_iterator();
  test_byte_buffer_compare();
  test_byte_buffer_clone();
  test_byte_buffer_move();
  test_byte_buffer_formatter();
}