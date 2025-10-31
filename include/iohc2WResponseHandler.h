/*
   Copyright (c) 2024. CRIDP https://github.com/cridp

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

           http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#ifndef IOHC2WRESPONSEHANDLER_H
#define IOHC2WRESPONSEHANDLER_H

#include <iohcPacket.h>
#include <iohcDevice2W.h>
#include <iohcRadio.h>

/**
 * @brief Handles 2W device responses (CMD 0x3C challenge and CMD 0x04 confirmation)
 * 
 * This class provides automatic handling of authentication challenges and status
 * responses from paired 2W devices outside of the pairing process.
 */
class IOHC2WResponseHandler {
public:
    /**
     * @brief Get the singleton instance
     */
    static IOHC2WResponseHandler* getInstance();
    
    /**
     * @brief Handle a CMD 0x3C challenge from a paired device
     * @param iohc The packet containing the challenge
     * @return true if the challenge was handled, false otherwise
     */
    bool handleChallenge(IOHC::iohcPacket* iohc);
    
    /**
     * @brief Handle a CMD 0x04 status/confirmation response
     * @param iohc The packet containing the response
     * @return true if the response was handled, false otherwise
     */
    bool handleConfirmation(IOHC::iohcPacket* iohc);
    
    /**
     * @brief Set the radio instance for sending authentication responses
     * @param radio The radio instance
     */
    void setRadioInstance(IOHC::iohcRadio* radio);

private:
    IOHC2WResponseHandler();
    static IOHC2WResponseHandler* _instance;
    IOHC::iohcRadio* _radioInstance;
};

#endif // IOHC2WRESPONSEHANDLER_H
