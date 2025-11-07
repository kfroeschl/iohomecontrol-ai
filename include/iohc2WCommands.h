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

#ifndef IOHC2WCOMMANDS_H
#define IOHC2WCOMMANDS_H

#include <interact.h>

namespace IOHC2WCommands {
    // Reusable 2W command implementation functions
    void pair2W(Tokens *cmd);
    void autoPair2W(Tokens *cmd);  // Discovery + automatic pairing
    void cancelPair2W(Tokens *cmd);
    void list2W(Tokens *cmd);
    void info2W(Tokens *cmd);
    void del2W(Tokens *cmd);
    void save2W(Tokens *cmd);
    void reload2W(Tokens *cmd);
    void on2W(Tokens *cmd);
    void off2W(Tokens *cmd);
    void status2W(Tokens *cmd);
    void test2W(Tokens *cmd);
}

#endif // IOHC2WCOMMANDS_H
