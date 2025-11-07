#include <unity.h>
// #include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <iohcCryptoHelpers.h>
#include <crypto2Wutils.h>
#include <vector>

void setUp(void) {
    // Serial.begin(115200);
}

void tearDown(void) {
    // clean stuff up here
}

// Helper function to build and verify a complete frame with CRC
std::string buildFrameWithCRC(const std::vector<uint8_t>& frameData) {
    std::vector<uint8_t> frame = frameData;
    uint16_t crc = iohcCrypto::radioPacketComputeCrc(frame);
    frame.push_back(crc & 0xff);
    frame.push_back((crc >> 8) & 0xff);
    return bytesToHexString(frame.data(), frame.size());
}

// Helper function to build frame and verify against expected value
void assertFrameEquals(const std::vector<uint8_t>& frameData, const char* expected, const char* description) {
    std::string result = buildFrameWithCRC(frameData);
    printf("  %s\n", result.c_str());
    TEST_ASSERT_EQUAL_STRING(expected, result.c_str());
}

// Helper function to encrypt 2W key (not present in original library)
void encrypt_2W_key(uint8_t* encrypted_key, const uint8_t* challenge, const std::vector<uint8_t>& frame_data, const uint8_t* key) {
    // Construct initial value using the frame data and challenge
    uint8_t initial_value[16] = {0};
    initial_value[8] = 0;
    initial_value[9] = 0;
    
    size_t i = 0;
    while (i < frame_data.size()) {
        Checksum checksum = computeChecksum(frame_data[i], initial_value[8], initial_value[9]);
        initial_value[8] = checksum.chksum1;
        initial_value[9] = checksum.chksum2;
        if (i < 8) {
            initial_value[i] = frame_data[i];
        }
        i++;
    }
    
    if (i < 8) {
        for (size_t j = i; j < 8; j++) {
            initial_value[j] = 0x55;
        }
    }
    
    // Add challenge to initial value
    for (i = 10; i < 16; i++) {
        initial_value[i] = challenge[i - 10];
    }
    
    // Encrypt the initial value with transfer key
    AES_ctx ctx;
    AES_init_ctx(&ctx, transfert_key);
    uint8_t encrypted_iv[16];
    memcpy(encrypted_iv, initial_value, 16);
    AES_ECB_encrypt(&ctx, encrypted_iv);
    
    // XOR with the system key
    for (i = 0; i < 16; i++) {
        encrypted_key[i] = encrypted_iv[i] ^ key[i];
    }
}

void test_1W_key_push() {
    printf("\n#### 1-way device key push using command 0x30, node address abcdef and sequence number 0x1234 ####\n");
    
    uint8_t node_address[3] = {0xab, 0xcd, 0xef};
    uint8_t sequence_number[2] = {0x12, 0x34};
    uint8_t controller_key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 
                                   0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    
    printf("Controller key in clear:\n");
    printf("  %s\n", bytesToHexString(controller_key, 16).c_str());
    TEST_ASSERT_EQUAL_STRING("01020304050607080910111213141516", bytesToHexString(controller_key, 16).c_str());
    
    // Encrypt the 1W key
    uint8_t encrypted_1W[16];
    memcpy(encrypted_1W, controller_key, 16);
    iohcCrypto::encrypt_1W_key(node_address, encrypted_1W);
    
    printf("Controller key encrypted:\n");
    printf("  %s\n", bytesToHexString(encrypted_1W, 16).c_str());
    TEST_ASSERT_EQUAL_STRING("7e60491f976adf653db0ed785e49a201", bytesToHexString(encrypted_1W, 16).c_str());
    
    // Build frame data (0x30 + encrypted key)
    std::vector<uint8_t> frame_data;
    frame_data.push_back(0x30);
    for (int i = 0; i < 16; i++) {
        frame_data.push_back(encrypted_1W[i]);
    }
    
    printf("Frame data:\n");
    printf("  %s\n", bytesToHexString(frame_data.data(), frame_data.size()).c_str());
    TEST_ASSERT_EQUAL_STRING("307e60491f976adf653db0ed785e49a201", bytesToHexString(frame_data.data(), frame_data.size()).c_str());
    
    // Create HMAC
    uint8_t hmac[16];
    iohcCrypto::create_1W_hmac(hmac, sequence_number, controller_key, frame_data);
    
    printf("Authentication message (first 6 bytes):\n");
    printf("  %s\n", bytesToHexString(hmac, 6).c_str());
    TEST_ASSERT_EQUAL_STRING("19e81ec43d5e", bytesToHexString(hmac, 6).c_str());
    
    // Build final frame
    std::vector<uint8_t> final_frame = {0xfc, 0x00, 0x00, 0x00, 0x3f};
    final_frame.insert(final_frame.end(), node_address, node_address + 3);
    final_frame.insert(final_frame.end(), frame_data.begin(), frame_data.end());
    final_frame.push_back(0x02);
    final_frame.push_back(0x01);
    final_frame.insert(final_frame.end(), sequence_number, sequence_number + 2);
    final_frame.insert(final_frame.end(), hmac, hmac + 6);
    
    printf("Final frame sent:\n");
    assertFrameEquals(final_frame, "fc0000003fabcdef307e60491f976adf653db0ed785e49a2010201123419e81ec43d5e9bf2", "Final frame with CRC");
}

void test_2W_key_pull() {
    printf("\n#### 2-way device key pull using command 0x38 and challenge 123456789abc ####\n");
    
    uint8_t challenge[6] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    uint8_t system_key[16] = {0xab, 0xcd, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05, 
                              0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13};
    
    // Build frame 0x38 + challenge
    std::vector<uint8_t> frame38;
    frame38.push_back(0x38);
    frame38.insert(frame38.end(), challenge, challenge + 6);
    
    printf("Encrypted 2-way key to be sent with 0x32:\n");
    uint8_t encrypted_2W[16];
    encrypt_2W_key(encrypted_2W, challenge, frame38, system_key);
    printf("  %s\n", bytesToHexString(encrypted_2W, 16).c_str());
    TEST_ASSERT_EQUAL_STRING("ea425a7a182885d4eaeefd416d625e01", bytesToHexString(encrypted_2W, 16).c_str());
    
    // Build frame 0x32 + encrypted key
    std::vector<uint8_t> frame32;
    frame32.push_back(0x32);
    frame32.insert(frame32.end(), encrypted_2W, encrypted_2W + 16);
    
    // Create challenge answer
    uint8_t mac_2w[16];
    iohcCrypto::create_2W_hmac(mac_2w, challenge, system_key, frame32);
    
    printf("Challenge answer to 123456789abc and last command 0x32:\n");
    printf("  %s\n", bytesToHexString(mac_2w, 6).c_str());
    TEST_ASSERT_EQUAL_STRING("0ae519a73c99", bytesToHexString(mac_2w, 6).c_str());
    
    printf("Frames sent on the air:\n");
    
    // 0x38 ask key transfer
    std::vector<uint8_t> frame = {0x4e, 0x04, 0xfe, 0xef, 0xee, 0xf0, 0x0f, 0x00};
    frame.insert(frame.end(), frame38.begin(), frame38.end());
    assertFrameEquals(frame, "4e04feefeef00f0038123456789abc23b6", "0x38 ask key transfer");
    
    // 0x32 key transfer
    frame = {0x18, 0x04, 0xf0, 0x0f, 0x00, 0xfe, 0xef, 0xee, 0x32};
    frame.insert(frame.end(), encrypted_2W, encrypted_2W + 16);
    assertFrameEquals(frame, "1804f00f00feefee32ea425a7a182885d4eaeefd416d625e016379", "0x32 key transfer");
    
    // 0x3c challenge
    frame = {0x0e, 0x00, 0xfe, 0xef, 0xee, 0xf0, 0x0f, 0x00, 0x3c};
    frame.insert(frame.end(), challenge, challenge + 6);
    assertFrameEquals(frame, "0e00feefeef00f003c123456789abc5eb1", "0x3c challenge");
    
    // 0x3d challenge answer
    frame = {0x8e, 0x00, 0xf0, 0x0f, 0x00, 0xfe, 0xef, 0xee, 0x3d};
    frame.insert(frame.end(), mac_2w, mac_2w + 6);
    assertFrameEquals(frame, "8e00f00f00feefee3d0ae519a73c992400", "0x3d challenge answer");
}

void test_2W_key_push() {
    printf("\n#### 2-way device key push using command 0x31 and challenge 123456789abc ####\n");
    
    uint8_t challenge1[6] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    uint8_t challenge2[6] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    uint8_t system_key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 
                              0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    
    // Build frame 0x31
    std::vector<uint8_t> frame31 = {0x31};
    
    printf("Encrypted 2-way key to be sent with 0x32:\n");
    uint8_t encrypted_2W[16];
    encrypt_2W_key(encrypted_2W, challenge1, frame31, system_key);
    printf("  %s\n", bytesToHexString(encrypted_2W, 16).c_str());
    TEST_ASSERT_EQUAL_STRING("102e49a16d3b69726f3192cf17534ad9", bytesToHexString(encrypted_2W, 16).c_str());
    
    // Build frame 0x32 with the specific encrypted key from Python demo
    uint8_t frame32_bytes[] = {0x32, 0xf8, 0x49, 0x58, 0x4f, 0xfc, 0xfc, 0x44, 0x2b, 
                               0x1e, 0x97, 0xe4, 0xc3, 0x8d, 0xf7, 0xb1, 0x43};
    std::vector<uint8_t> frame32(frame32_bytes, frame32_bytes + 17);
    
    // Create challenge answer for frame32
    uint8_t mac_2w32[16];
    iohcCrypto::create_2W_hmac(mac_2w32, challenge2, system_key, frame32);
    
    printf("Challenge answer to challenge 123456789abc and last command 0x32:\n");
    printf("  %s\n", bytesToHexString(mac_2w32, 6).c_str());
    TEST_ASSERT_EQUAL_STRING("8dc9d40dc7a4", bytesToHexString(mac_2w32, 6).c_str());
    
    printf("Frames sent on the air:\n");
    
    // 0x31 ask challenge
    std::vector<uint8_t> frame = {0x48, 0x00, 0xfe, 0xef, 0xee, 0xf0, 0x0f, 0x00, 0x31};
    assertFrameEquals(frame, "4800feefeef00f0031fb60", "0x31 ask challenge");
    
    // 0x3c challenge
    frame = {0x0e, 0x00, 0xf0, 0x0f, 0x00, 0xfe, 0xef, 0xee, 0x3c};
    frame.insert(frame.end(), challenge1, challenge1 + 6);
    assertFrameEquals(frame, "0e00f00f00feefee3c123456789abc19db", "0x3c challenge 1");
    
    // 0x32 key transfer
    frame = {0x18, 0x00, 0xf0, 0x0f, 0x00, 0xfe, 0xef, 0xee, 0x32};
    frame.insert(frame.end(), encrypted_2W, encrypted_2W + 16);
    assertFrameEquals(frame, "1800f00f00feefee32102e49a16d3b69726f3192cf17534ad98043", "0x32 key transfer");
    
    // 0x3c challenge
    frame = {0x0e, 0x00, 0xf0, 0x0f, 0x00, 0xfe, 0xef, 0xee, 0x3c};
    frame.insert(frame.end(), challenge2, challenge2 + 6);
    assertFrameEquals(frame, "0e00f00f00feefee3c123456789abc19db", "0x3c challenge 2");
    
    // 0x3d challenge answer
    frame = {0x0e, 0x00, 0xfe, 0xef, 0xee, 0xf0, 0x0f, 0x00, 0x3d};
    frame.insert(frame.end(), mac_2w32, mac_2w32 + 6);
    assertFrameEquals(frame, "0e00feefeef00f003d8dc9d40dc7a4f9e5", "0x3d challenge answer");
    
    // 0x33 key transfer complete
    frame = {0x88, 0x00, 0xf0, 0x0f, 0x00, 0xfe, 0xef, 0xee, 0x33};
    assertFrameEquals(frame, "8800f00f00feefee335bfb", "0x33 key transfer complete");
}

int main( int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_1W_key_push);
    RUN_TEST(test_2W_key_pull);
    RUN_TEST(test_2W_key_push);
    UNITY_END();
}