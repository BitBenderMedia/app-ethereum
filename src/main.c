/*******************************************************************************
*   Ledger Ethereum App
*   (c) 2016-2019 Ledger
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include "shared_context.h"
#include "apdu_constants.h"
#include "ui_callbacks.h"
#include "ui_flow.h"

#include "os_io_seproxyhal.h"

#include "glyphs.h"
#include "utils.h"

#include "swap_lib_calls.h"
#include "handle_swap_sign_transaction.h"
#include "handle_get_printable_amount.h"
#include "handle_check_address.h"

#ifdef HAVE_STARKWARE
#include "stark_crypto.h"
#endif

unsigned char G_io_seproxyhal_spi_buffer[IO_SEPROXYHAL_BUFFER_SIZE_B];

void ui_idle(void);

uint32_t set_result_get_publicKey(void);
void finalizeParsing(bool);

tmpCtx_t tmpCtx;
txContext_t txContext;
tmpContent_t tmpContent;
dataContext_t dataContext;
strings_t strings;
cx_sha3_t global_sha3;

uint8_t appState;
bool dataPresent;
bool called_from_swap;
#ifdef HAVE_STARKWARE
bool quantumSet;
#endif
#ifdef HAVE_ETH2
uint32_t eth2WithdrawalIndex;
#endif

#include "ux.h"
ux_state_t G_ux;
bolos_ux_params_t G_ux_params;

const internalStorage_t N_storage_real;

chain_config_t *chainConfig;

void reset_app_context() {
  //PRINTF("!!RESET_APP_CONTEXT\n");
  appState = APP_STATE_IDLE;
  os_memset(tmpCtx.transactionContext.tokenSet, 0, MAX_TOKEN);
  called_from_swap = false;
#ifdef HAVE_STARKWARE
  quantumSet = false;
#endif
#ifdef HAVE_ETH2
  eth2WithdrawalIndex = 0;
#endif
  os_memset((uint8_t*)&txContext, 0, sizeof(txContext));
  os_memset((uint8_t*)&tmpContent, 0, sizeof(tmpContent));
}

void ui_idle(void) {
    // reserve a display stack slot if none yet
    if(G_ux.stack_count == 0) {
        ux_stack_push();
    }
    ux_flow_init(0, ux_idle_flow, NULL);
}

unsigned int io_seproxyhal_touch_exit(const bagl_element_t *e) {
    // Go back to the dashboard
    os_sched_exit(0);
    return 0; // do not redraw the widget
}

#if defined(TARGET_NANOS)
unsigned int ui_address_nanos_button(unsigned int button_mask, unsigned int button_mask_counter) {
    switch(button_mask) {
        case BUTTON_EVT_RELEASED|BUTTON_LEFT: // CANCEL
			      io_seproxyhal_touch_address_cancel(NULL);
            break;

        case BUTTON_EVT_RELEASED|BUTTON_RIGHT: { // OK
			      io_seproxyhal_touch_address_ok(NULL);
			      break;
        }
    }
    return 0;
}
#endif // #if defined(TARGET_NANOS)

void io_seproxyhal_send_status(uint32_t sw) {
    G_io_apdu_buffer[0] = ((sw >> 8) & 0xff);
    G_io_apdu_buffer[1] = (sw & 0xff);
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
}

void format_signature_out(const uint8_t* signature) {
  os_memset(G_io_apdu_buffer + 1, 0x00, 64);
  uint8_t offset = 1;
  uint8_t xoffset = 4; //point to r value
  //copy r
  uint8_t xlength = signature[xoffset-1];
  if (xlength == 33) {
    xlength = 32;
    xoffset ++;
  }
  memmove(G_io_apdu_buffer+offset+32-xlength,  signature+xoffset, xlength);
  offset += 32;
  xoffset += xlength +2; //move over rvalue and TagLEn
  //copy s value
  xlength = signature[xoffset-1];
  if (xlength == 33) {
    xlength = 32;
    xoffset ++;
  }
  memmove(G_io_apdu_buffer+offset+32-xlength, signature+xoffset, xlength);
}

unsigned short io_exchange_al(unsigned char channel, unsigned short tx_len) {
    switch (channel & ~(IO_FLAGS)) {
    case CHANNEL_KEYBOARD:
        break;

    // multiplexed io exchange over a SPI channel and TLV encapsulated protocol
    case CHANNEL_SPI:
        if (tx_len) {
            io_seproxyhal_spi_send(G_io_apdu_buffer, tx_len);

            if (channel & IO_RESET_AFTER_REPLIED) {
                reset();
            }
            return 0; // nothing received from the master so far (it's a tx
                      // transaction)
        } else {
            return io_seproxyhal_spi_recv(G_io_apdu_buffer,
                                          sizeof(G_io_apdu_buffer), 0);
        }

    default:
        THROW(INVALID_PARAMETER);
    }
    return 0;
}

tokenDefinition_t* getKnownToken(uint8_t *contractAddress) {
    tokenDefinition_t *currentToken = NULL;
#ifdef HAVE_TOKENS_LIST
    uint32_t numTokens = 0;
    uint32_t i;
    switch(chainConfig->kind) {
        case CHAIN_KIND_AKROMA:
            numTokens = NUM_TOKENS_AKROMA;
            break;
        case CHAIN_KIND_ETHEREUM:
            numTokens = NUM_TOKENS_ETHEREUM;
            break;
        case CHAIN_KIND_ETHEREUM_CLASSIC:
            numTokens = NUM_TOKENS_ETHEREUM_CLASSIC;
            break;
        case CHAIN_KIND_PIRL:
            numTokens = NUM_TOKENS_PIRL;
            break;
        case CHAIN_KIND_POA:
            numTokens = NUM_TOKENS_POA;
            break;
        case CHAIN_KIND_ARTIS_SIGMA1:
            numTokens = NUM_TOKENS_ARTIS_SIGMA1;
            break;
        case CHAIN_KIND_ARTIS_TAU1:
            numTokens = NUM_TOKENS_ARTIS_TAU1;
            break;
        case CHAIN_KIND_RSK:
            numTokens = NUM_TOKENS_RSK;
            break;
        case CHAIN_KIND_EXPANSE:
            numTokens = NUM_TOKENS_EXPANSE;
            break;
        case CHAIN_KIND_UBIQ:
            numTokens = NUM_TOKENS_UBIQ;
            break;
        case CHAIN_KIND_WANCHAIN:
            numTokens = NUM_TOKENS_WANCHAIN;
            break;
        case CHAIN_KIND_KUSD:
            numTokens = NUM_TOKENS_KUSD;
            break;
        case CHAIN_KIND_MUSICOIN:
            numTokens = NUM_TOKENS_MUSICOIN;
            break;
        case CHAIN_KIND_CALLISTO:
            numTokens = NUM_TOKENS_CALLISTO;
            break;
        case CHAIN_KIND_ETHERSOCIAL:
            numTokens = NUM_TOKENS_ETHERSOCIAL;
            break;
        case CHAIN_KIND_ELLAISM:
            numTokens = NUM_TOKENS_ELLAISM;
            break;
        case CHAIN_KIND_ETHER1:
            numTokens = NUM_TOKENS_ETHER1;
            break;
        case CHAIN_KIND_ETHERGEM:
            numTokens = NUM_TOKENS_ETHERGEM;
            break;
        case CHAIN_KIND_ATHEIOS:
            numTokens = NUM_TOKENS_ATHEIOS;
            break;
        case CHAIN_KIND_GOCHAIN:
            numTokens = NUM_TOKENS_GOCHAIN;
            break;
        case CHAIN_KIND_MIX:
            numTokens = NUM_TOKENS_MIX;
            break;
        case CHAIN_KIND_REOSC:
            numTokens = NUM_TOKENS_REOSC;
            break;
        case CHAIN_KIND_HPB:
            numTokens = NUM_TOKENS_HPB;
            break;
        case CHAIN_KIND_TOMOCHAIN:
            numTokens = NUM_TOKENS_TOMOCHAIN;
            break;
        case CHAIN_KIND_TOBALABA:
            numTokens = NUM_TOKENS_TOBALABA;
            break;
        case CHAIN_KIND_DEXON:
            numTokens = NUM_TOKENS_DEXON;
            break;
        case CHAIN_KIND_VOLTA:
            numTokens = NUM_TOKENS_VOLTA;
            break;
        case CHAIN_KIND_EWC:
            numTokens = NUM_TOKENS_EWC;
            break;
        case CHAIN_KIND_WEBCHAIN:
            numTokens = NUM_TOKENS_WEBCHAIN;
            break;
        case CHAIN_KIND_THUNDERCORE:
            numTokens = NUM_TOKENS_THUNDERCORE;
            break;
    }
    for (i=0; i<numTokens; i++) {
        switch(chainConfig->kind) {
            case CHAIN_KIND_AKROMA:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_AKROMA[i]);
                break;
            case CHAIN_KIND_ETHEREUM:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ETHEREUM[i]);
                break;
            case CHAIN_KIND_ETHEREUM_CLASSIC:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ETHEREUM_CLASSIC[i]);
                break;
            case CHAIN_KIND_PIRL:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_PIRL[i]);
                break;
            case CHAIN_KIND_POA:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_POA[i]);
                break;
            case CHAIN_KIND_ARTIS_SIGMA1:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ARTIS_SIGMA1[i]);
                break;
            case CHAIN_KIND_ARTIS_TAU1:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ARTIS_TAU1[i]);
                break;
            case CHAIN_KIND_RSK:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_RSK[i]);
                break;
            case CHAIN_KIND_EXPANSE:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_EXPANSE[i]);
                break;
            case CHAIN_KIND_UBIQ:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_UBIQ[i]);
                break;
            case CHAIN_KIND_WANCHAIN:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_WANCHAIN[i]);
                break;
            case CHAIN_KIND_KUSD:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_KUSD[i]);
                break;
            case CHAIN_KIND_MUSICOIN:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_MUSICOIN[i]);
                break;
            case CHAIN_KIND_CALLISTO:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_CALLISTO[i]);
                break;
            case CHAIN_KIND_ETHERSOCIAL:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ETHERSOCIAL[i]);
                break;
            case CHAIN_KIND_ELLAISM:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ELLAISM[i]);
                break;
            case CHAIN_KIND_ETHER1:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ETHER1[i]);
                break;
            case CHAIN_KIND_ETHERGEM:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ETHERGEM[i]);
                break;
            case CHAIN_KIND_ATHEIOS:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_ATHEIOS[i]);
                break;
            case CHAIN_KIND_GOCHAIN:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_GOCHAIN[i]);
                break;
            case CHAIN_KIND_MIX:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_MIX[i]);
                break;
            case CHAIN_KIND_REOSC:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_REOSC[i]);
                break;
            case CHAIN_KIND_HPB:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_HPB[i]);
                break;
            case CHAIN_KIND_TOMOCHAIN:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_TOMOCHAIN[i]);
                break;
            case CHAIN_KIND_TOBALABA:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_TOBALABA[i]);
                break;
            case CHAIN_KIND_DEXON:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_DEXON[i]);
                break;
            case CHAIN_KIND_VOLTA:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_VOLTA[i]);
                break;
            case CHAIN_KIND_EWC:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_EWC[i]);
                break;
            case CHAIN_KIND_WEBCHAIN:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_WEBCHAIN[i]);
                break;
            case CHAIN_KIND_THUNDERCORE:
                currentToken = (tokenDefinition_t *)PIC(&TOKENS_THUNDERCORE[i]);
                break
        }
        if (os_memcmp(currentToken->address, tmpContent.txContent.destination, 20) == 0) {
            return currentToken;
        }
    }
#endif
    for(size_t i=0; i<MAX_TOKEN; i++){
      currentToken = &tmpCtx.transactionContext.tokens[i];
      if (tmpCtx.transactionContext.tokenSet[i] && (os_memcmp(currentToken->address, contractAddress, 20) == 0)) {
        PRINTF("Token found at index %d\n", i);
        return currentToken;
      }
    }

    return NULL;
}

void handleApdu(unsigned int *flags, unsigned int *tx) {
  unsigned short sw = 0;

  BEGIN_TRY {
    TRY {

#ifdef HAVE_STARKWARE

      if (G_io_apdu_buffer[OFFSET_CLA] == STARKWARE_CLA) {
        switch(G_io_apdu_buffer[OFFSET_INS]) {
          case STARKWARE_INS_GET_PUBLIC_KEY:
            handleStarkwareGetPublicKey(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
            break;
          case STARKWARE_INS_SIGN_MESSAGE:
            handleStarkwareSignMessage(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
            break;
          case STARKWARE_INS_PROVIDE_QUANTUM:
            handleStarkwareProvideQuantum(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
            break;
          default:
            THROW(0x6D00);
            break;
        }
        CLOSE_TRY;
        return;
      }

#endif

      if (G_io_apdu_buffer[OFFSET_CLA] != CLA) {
        THROW(0x6E00);
      }

      switch (G_io_apdu_buffer[OFFSET_INS]) {
        case INS_GET_PUBLIC_KEY:
          os_memset(tmpCtx.transactionContext.tokenSet, 0, MAX_TOKEN);
          handleGetPublicKey(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
          break;

        case INS_PROVIDE_ERC20_TOKEN_INFORMATION:
          handleProvideErc20TokenInformation(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
          break;

        case INS_SIGN:
          handleSign(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
          break;

        case INS_GET_APP_CONFIGURATION:
          handleGetAppConfiguration(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
          break;

        case INS_SIGN_PERSONAL_MESSAGE:
          os_memset(tmpCtx.transactionContext.tokenSet, 0, MAX_TOKEN);
          handleSignPersonalMessage(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
          break;

        case INS_SIGN_EIP_712_MESSAGE:
           os_memset(tmpCtx.transactionContext.tokenSet, 0, MAX_TOKEN);
           handleSignEIP712Message(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
           break;

#ifdef HAVE_ETH2

        case INS_GET_ETH2_PUBLIC_KEY:
           os_memset(tmpCtx.transactionContext.tokenSet, 0, MAX_TOKEN);
           handleGetEth2PublicKey(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
           break;

        case INS_SET_ETH2_WITHDRAWAL_INDEX:
            handleSetEth2WithdrawalIndex(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], flags, tx);
            break;

#endif

#if 0
        case 0xFF: // return to dashboard
          goto return_to_dashboard;
#endif

        default:
          THROW(0x6D00);
          break;
      }
    }
    CATCH(EXCEPTION_IO_RESET) {
      THROW(EXCEPTION_IO_RESET);
    }
    CATCH_OTHER(e) {
      switch (e & 0xF000) {
        case 0x6000:
          // Wipe the transaction context and report the exception
          sw = e;
          reset_app_context();
          break;
        case 0x9000:
          // All is well
          sw = e;
          break;
        default:
          // Internal error
          sw = 0x6800 | (e & 0x7FF);
          reset_app_context();
          break;
        }
        // Unexpected exception => report
        G_io_apdu_buffer[*tx] = sw >> 8;
        G_io_apdu_buffer[*tx + 1] = sw;
        *tx += 2;
      }
      FINALLY {
      }
  }
  END_TRY;
}

void app_main(void) {
    unsigned int rx = 0;
    unsigned int tx = 0;
    unsigned int flags = 0;

    // DESIGN NOTE: the bootloader ignores the way APDU are fetched. The only
    // goal is to retrieve APDU.
    // When APDU are to be fetched from multiple IOs, like NFC+USB+BLE, make
    // sure the io_event is called with a
    // switch event, before the apdu is replied to the bootloader. This avoid
    // APDU injection faults.
    for (;;) {
        unsigned short sw = 0;

        BEGIN_TRY {
            TRY {
                rx = tx;
                tx = 0; // ensure no race in catch_other if io_exchange throws
                        // an error
                rx = io_exchange(CHANNEL_APDU | flags, rx);
                flags = 0;

                // no apdu received, well, reset the session, and reset the
                // bootloader configuration
                if (rx == 0) {
                    THROW(0x6982);
                }
                PRINTF("New APDU received:\n%.*H\n", rx, G_io_apdu_buffer);

                handleApdu(&flags, &tx);
            }
            CATCH(EXCEPTION_IO_RESET) {
                THROW(EXCEPTION_IO_RESET);
            }
            CATCH_OTHER(e) {
                switch (e & 0xF000) {
                case 0x6000:
                    // Wipe the transaction context and report the exception
                    sw = e;
                    reset_app_context();
                    break;
                case 0x9000:
                    // All is well
                    sw = e;
                    break;
                default:
                    // Internal error
                    sw = 0x6800 | (e & 0x7FF);
                    reset_app_context();
                    break;
                }
                if (e != 0x9000) {
                    flags &= ~IO_ASYNCH_REPLY;
                }
                // Unexpected exception => report
                G_io_apdu_buffer[tx] = sw >> 8;
                G_io_apdu_buffer[tx + 1] = sw;
                tx += 2;
            }
            FINALLY {
            }
        }
        END_TRY;
    }

//return_to_dashboard:
    return;
}

// override point, but nothing more to do
void io_seproxyhal_display(const bagl_element_t *element) {
  io_seproxyhal_display_default((bagl_element_t *)element);
}

unsigned char io_event(unsigned char channel) {
    // nothing done with the event, throw an error on the transport layer if
    // needed

    // can't have more than one tag in the reply, not supported yet.
    switch (G_io_seproxyhal_spi_buffer[0]) {
    case SEPROXYHAL_TAG_FINGER_EVENT:
    		UX_FINGER_EVENT(G_io_seproxyhal_spi_buffer);
    		break;

    case SEPROXYHAL_TAG_BUTTON_PUSH_EVENT:
        UX_BUTTON_PUSH_EVENT(G_io_seproxyhal_spi_buffer);
        break;

    case SEPROXYHAL_TAG_STATUS_EVENT:
        if (G_io_apdu_media == IO_APDU_MEDIA_USB_HID && !(U4BE(G_io_seproxyhal_spi_buffer, 3) & SEPROXYHAL_TAG_STATUS_EVENT_FLAG_USB_POWERED)) {
         THROW(EXCEPTION_IO_RESET);
        }
        // no break is intentional
    default:
        UX_DEFAULT_EVENT();
        break;

    case SEPROXYHAL_TAG_DISPLAY_PROCESSED_EVENT:
        UX_DISPLAYED_EVENT({});
        break;

#if 0
    case SEPROXYHAL_TAG_TICKER_EVENT:
        UX_TICKER_EVENT(G_io_seproxyhal_spi_buffer,
        {
        });
        break;
#endif
    }

    // close the event if not done previously (by a display or whatever)
    if (!io_seproxyhal_spi_is_status_sent()) {
        io_seproxyhal_general_status();
    }

    // command has been processed, DO NOT reset the current APDU transport
    return 1;
}

void app_exit() {

    BEGIN_TRY_L(exit) {
        TRY_L(exit) {
            os_sched_exit(-1);
        }
        FINALLY_L(exit) {

        }
    }
    END_TRY_L(exit);
}


void coin_main_with_config(chain_config_t *config) {

    chainConfig = config;
    reset_app_context();
    tmpCtx.transactionContext.currentTokenIndex = 0;

    for (;;) {
        UX_INIT();

        BEGIN_TRY {
            TRY {
                io_seproxyhal_init();

#ifdef TARGET_NANOX
                // grab the current plane mode setting
                G_io_app.plane_mode = os_setting_get(OS_SETTING_PLANEMODE, NULL, 0);
#endif // TARGET_NANOX

                if (N_storage.initialized != 0x01) {
                    internalStorage_t storage;
                    storage.dataAllowed = 0x00;
                    storage.contractDetails = 0x00;
                    storage.initialized = 0x01;
                    nvm_write((void*)&N_storage, (void*)&storage, sizeof(internalStorage_t));
                }

                USB_power(0);
                USB_power(1);

                ui_idle();

#ifdef HAVE_BLE
                BLE_power(0, NULL);
                BLE_power(1, "Nano X");
#endif // HAVE_BLE

                app_main();
            }
            CATCH(EXCEPTION_IO_RESET) {
                // reset IO and UX before continuing
                CLOSE_TRY;
                continue;
            }
            CATCH_ALL {
                CLOSE_TRY;
                break;
            }
            FINALLY {
            }
        }
        END_TRY;
    }
    app_exit();
}

void init_coin_config(chain_config_t *coin_config) {
    os_memset(coin_config, 0, sizeof(chain_config_t));
    strcpy(coin_config->coinName, CHAINID_COINNAME " ");
    coin_config->chainId = CHAIN_ID;
    coin_config->kind = CHAIN_KIND;
}

void coin_main() {
    chain_config_t coin_config;
    init_coin_config(&coin_config);
    coin_main_with_config(&coin_config);
}

void library_main_with_config(chain_config_t *config, unsigned int command, unsigned int* call_parameters) {
    BEGIN_TRY {
        TRY {
            check_api_level(CX_COMPAT_APILEVEL);
            PRINTF("Inside a library \n");
            switch (command) {
                case CHECK_ADDRESS:
                    handle_check_address((check_address_parameters_t*)call_parameters, config);
                break;
                case SIGN_TRANSACTION:
                    handle_swap_sign_transaction((create_transaction_parameters_t*)call_parameters, config);
                break;
                case GET_PRINTABLE_AMOUNT:
                    handle_get_printable_amount((get_printable_amount_parameters_t*)call_parameters, config);
                break;
            }
            os_lib_end();
        }
        FINALLY {}
    }
    END_TRY;
}

void library_main(unsigned int call_id, unsigned int* call_parameters) {
    chain_config_t coin_config;
    init_coin_config(&coin_config);
    library_main_with_config(&coin_config, call_id, call_parameters);
}

__attribute__((section(".boot"))) int main(int arg0) {
#ifdef USE_LIB_ETHEREUM
    BEGIN_TRY {
        TRY {
            unsigned int libcall_params[5];
            chain_config_t local_chainConfig;
            init_coin_config(&local_chainConfig);
            PRINTF("Hello from Eth-clone\n");
            check_api_level(CX_COMPAT_APILEVEL);
            // delegate to Ethereum app/lib
            libcall_params[0] = "Ethereum";
            libcall_params[1] = 0x100;
            libcall_params[2] = RUN_APPLICATION;
            libcall_params[3] = &local_chainConfig;
            libcall_params[4] = 0;
            if (arg0) {
                // call as a library
                libcall_params[2] = ((unsigned int *)arg0)[1];
                libcall_params[4] = ((unsigned int *)arg0)[3]; // library arguments
                os_lib_call(&libcall_params);
                ((unsigned int *)arg0)[0] = libcall_params[1];
                os_lib_end();
            }
            else {
                // launch coin application
                libcall_params[1] = 0x100; // use the Init call, as we won't exit
                os_lib_call(&libcall_params);
            }
        }
        FINALLY {}
    }
    END_TRY;
            // no return
#else
    // exit critical section
    __asm volatile("cpsie i");

    // ensure exception will work as planned
    os_boot();

    if (!arg0) {
        // called from dashboard as standalone eth app
        coin_main();
        return 0;
    }

    if (((unsigned int *)arg0)[0] != 0x100) {
        app_exit();
        return 0;
    }
    unsigned int command = ((unsigned int *)arg0)[1];
    chain_config_t * chain_config = ((unsigned int *)arg0)[2];
    switch (command) {
        case RUN_APPLICATION:
            // coin application launched from dashboard
            if (chain_config == NULL)
                app_exit();
            else
                coin_main_with_config(chain_config);
        break;
        default:
            if (chain_config == NULL)
                // Called as standalone eth library
                library_main(command, ((unsigned int *)arg0)[3]);// called as bitcoin library
            else
                // Called as a library from an altcoin
                library_main_with_config(chain_config, command, ((unsigned int *)arg0)[3]);// called as coin library
        break;
    }
#endif
    return 0;
}









