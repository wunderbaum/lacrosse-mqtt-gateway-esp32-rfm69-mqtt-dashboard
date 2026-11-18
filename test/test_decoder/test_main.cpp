#include <unity.h>
#include "LaCrosseDecoder.h"

static uint8_t testCrc(const uint8_t *data, uint8_t len) {
  uint8_t crc=0;
  for(uint8_t j=0;j<len;++j){uint8_t v=data[j];for(uint8_t i=0;i<8;++i){bool mix=((crc^v)&0x80)!=0;crc<<=1;if(mix)crc^=0x31;v<<=1;}}
  return crc;
}
static void makeFrame(uint8_t id,float temp,uint8_t humidity,bool low,bool fresh,uint8_t out[5]) {
  uint16_t value=uint16_t((temp+40.0f)*10.0f+0.5f);
  out[0]=uint8_t(0x90|(id>>2));
  out[1]=uint8_t((id&3)<<6)|(fresh?0x20:0)|(value/100);
  out[2]=uint8_t(((value/10)%10)<<4)|(value%10);
  out[3]=uint8_t(humidity&0x7f)|(low?0x80:0);
  out[4]=testCrc(out,4);
}
static void testDecodeValidFrame(){uint8_t b[5];makeFrame(56,18.0f,37,false,true,b);LaCrosseReading r;TEST_ASSERT_TRUE(decodeLaCrosseFrame(b,r));TEST_ASSERT_EQUAL_UINT8(56,r.id);TEST_ASSERT_FLOAT_WITHIN(0.01f,18.0f,r.temperature);TEST_ASSERT_EQUAL_UINT8(37,r.humidity);TEST_ASSERT_FALSE(r.lowBattery);TEST_ASSERT_TRUE(r.newBattery);}
static void testDecodeLowBattery(){uint8_t b[5];makeFrame(12,-5.3f,89,true,false,b);LaCrosseReading r;TEST_ASSERT_TRUE(decodeLaCrosseFrame(b,r));TEST_ASSERT_EQUAL_UINT8(12,r.id);TEST_ASSERT_FLOAT_WITHIN(0.01f,-5.3f,r.temperature);TEST_ASSERT_TRUE(r.lowBattery);}
static void testRejectBadCrc(){uint8_t b[5];makeFrame(56,18.0f,37,false,false,b);b[4]^=1;LaCrosseReading r;TEST_ASSERT_FALSE(decodeLaCrosseFrame(b,r));}
static void testRejectBadHeader(){uint8_t b[5];makeFrame(56,18.0f,37,false,false,b);b[0]^=0x10;b[4]=testCrc(b,4);LaCrosseReading r;TEST_ASSERT_FALSE(decodeLaCrosseFrame(b,r));}
static void testRejectInvalidBcd(){uint8_t b[5];makeFrame(56,18.0f,37,false,false,b);b[1]=(b[1]&0xf0)|0x0a;b[4]=testCrc(b,4);LaCrosseReading r;TEST_ASSERT_FALSE(decodeLaCrosseFrame(b,r));}
void setup(){UNITY_BEGIN();RUN_TEST(testDecodeValidFrame);RUN_TEST(testDecodeLowBattery);RUN_TEST(testRejectBadCrc);RUN_TEST(testRejectBadHeader);RUN_TEST(testRejectInvalidBcd);UNITY_END();}
void loop(){}
