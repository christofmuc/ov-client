#include "ov_client_digitalstage.h"
#include <boost/filesystem.hpp>
#include <cpprest/filestream.h>
#include <cpprest/http_client.h>
#include <cpprest/json.h>
#include <cpprest/uri.h>
#include <cpprest/ws_client.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <udpsocket.h>

using namespace utility;
using namespace web;
using namespace web::http;
using namespace web::http::client;
using namespace utility::conversions;
using namespace web::websockets::client;
using namespace pplx;
using namespace concurrency::streams;

using json = nlohmann::json;

boost::filesystem::path ds_config_path;

// auth data
std::string email;
std::string password;
std::string jwt;

nlohmann::json user; // jsonObject for user data

nlohmann::json stage;         // jsonObject for joined d-s stage
nlohmann::json stage_members; // jsonObject containing joined d-s stage-members
nlohmann::json stages;        // jsonObject containing d-s stages
nlohmann::json groups;        // jsonObject containing d-s groups
nlohmann::json track_presets; // jsonObject containing d-s track presets
nlohmann::json sound_cards;   // jsonObject array with sound_cards

task_completion_event<void> tce; // used to terminate async PPLX listening task
websocket_callback_client wsclient;

ov_client_digitalstage_t::ov_client_digitalstage_t(
    ov_render_base_t& backend, const std::string& frontend_url_,
    boost::filesystem::path& selfpath)
    : ov_client_base_t(backend), runservice(true), frontend_url(frontend_url_),
      quitrequest_(false)
{
  std::cout << "app path " << selfpath << std::endl;
  selfpath = selfpath.remove_filename();
  selfpath = selfpath.append("ds-config");
  ds_config_path = selfpath;

  std::cout << "ds-config file path " << selfpath << std::endl;
  if(boost::filesystem::exists(selfpath)) {
    std::cout << "digital-stage config file found " << std::endl;
  }
}

void ov_client_digitalstage_t::start_service()
{
  runservice = true;
  servicethread = std::thread(&ov_client_digitalstage_t::service, this);
}

void ov_client_digitalstage_t::stop_service()
{
  runservice = false;
  tce.set();        // task completion event is set closing wss listening task
  wsclient.close(); // wss client is closed
  servicethread.join(); // thread is joined
}

void ov_client_digitalstage_t::service()
{
  // register_device(lobby, backend.get_deviceid());
  // download_file(lobby + "/announce.flac", "announce.flac");
  // start main control loop:

  ucout << "CHECK - digital-stage config file " << std::endl;

  if(boost::filesystem::exists("ds-config")) {
    std::cout << "OK - digital-stage config file FOUND  " << std::endl;

    std::string line;
    std::ifstream myfile("ds-config");

    if(myfile.is_open()) {
      std::getline(myfile, email);
      std::getline(myfile, password);
      // std::cout << email << "   " << password << '\n';

      myfile.close();
      if(email == "email") {
        std::cout << "Please provide email in ds-config file line 0" << '\n';
        quitrequest_ = true;
        return;
      }
      if(password == "password") {
        std::cout << "Please provide password in ds-config file line 1" << '\n';
        quitrequest_ = true;
        return;
      }
    }

    else {
      std::cout << "Unable to open ds-config file";
      quitrequest_ = true;
      return;
    }
  }

  std::string url_ = "https://auth.digital-stage.org/login?email=" + email +
                     "&password=" + password;
  http_client client1(utility::conversions::to_string_t(url_));
  http_request request;
  request.set_method(methods::POST);
  request.headers().add(U("Host"), U("auth.digital-stage.org"));
  // request.headers().add(U("Origin"), U("https://test.digital-stage.org"));
  request.headers().add(U("Content-Type"), U("application/json"));

  client1.request(request)
      .then([](http_response response) {
        // std::cout<<"Response code is : "<<response.status_code();
        // std::cout<<"Response body is : "<<response.body();

        std::string str = response.extract_json().get().as_string();

        str.substr(1, str.length() - 1);
        jwt = str;
        std::cout << "jwt : " << jwt << std::endl;
      })
      .wait();

  wsclient.connect(U("wss://api.digital-stage.org")).wait();

  auto receive_task = create_task(tce);

  // handler for incoming d-s messages
  wsclient.set_message_handler([&](websocket_incoming_message ret_msg) {
    // -----------------------------------------------
    // -----    parse incoming message events    -----
    // -----------------------------------------------

    auto ret_str = ret_msg.extract_string().get();
    // ucout << "ret_str " << to_string_t(ret_str) << "\n";

    // we check if it's valid json
    // if(!nlohmann::json::accept(ret_str)) {
    //  std::cerr << "parse error" << std::endl;
    //}

    if(ret_str == "hey") {
      ucout << "ret_str " << to_string_t(ret_str) << "\n";
    } else {
      try {
        nlohmann::json j = nlohmann::json::parse(ret_str);
        // ucout << "/----------------------Event--------------------------/"
        //          << std::endl;
        // ucout << j["data"].dump(4) << std::endl;

        // 88   88 .dP"Y8 888888 88""Yb
        // 88   88 `Ybo." 88__   88__dP
        // Y8   8P o.`Y8b 88""   88"Yb
        //`YbodP' 8bodP' 888888 88  Yb

        // ----------------------------
        // -----    user ready    -----
        // ----------------------------

        if(j["data"][0] == "user-ready") {
          ucout << "\n/--  USER_READY --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;

          // we get user data from incoming json event message
          // and put the data into user json object for later use
          user = j["data"][1];
          // we can get single json entry fron nlohmann::json like this:
          ucout << "_id:  " << user["_id"] << std::endl;

          // or we can dump nlohmann::json objects like this:
          ucout << "user:  " << user.dump(4) << std::endl;
        }

        // ------------------------------
        // -----    user-changed    -----
        // ------------------------------

        if(j["data"][0] == "user-changed") {
          ucout << "\n/--  USER_CHANGED --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;

          ucout << "old user: \n" + user.dump(4) << std::endl;

          // we shall here manipulate the user jsonObject
          // to get new data in
        }

        // 8888b.  888888 Yb    dP 88  dP""b8 888888
        // 8I  Yb 88__    Yb  dP  88 dP   `" 88__
        // 8I  dY 88""     YbdP   88 Yb      88""
        // 8888Y"  888888    YP    88  YboodP 888888

        // ------------------------------------
        // -----    local-device-ready    -----
        // ------------------------------------

        if(j["data"][0] == "local-device-ready") {
          ucout << "\n/--  LOCAL_DEVICE_READY --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;
        }

        // ----------------------------------
        // -----    sound-card-added    -----
        // ----------------------------------

        if(j["data"][0] == "sound-card-added") {
          ucout << "\n/--  SOUND_CARD_ADDED --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;

          // we add the sound-card to sound_cards inMemory array jsonObject
          sound_cards.push_back(j["data"][1]);

          // print sound_cards jsonObject array
          ucout << "sound_cards:\n" << sound_cards.dump(4) << std::endl;
        }
        // ------------------------------
        // -----    device-added    -----
        // ------------------------------

        if(j["data"][0] == "device-added") {
          ucout << "\n/--  DEVICE_ADDED --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;
        }

        // --------------------------------
        // -----    device-changed    -----
        // --------------------------------
        if(j["data"][0] == "device-changed") {
          ucout << "\n/--  DEVICE_CHANGED --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;

          // TODO:manage device-changed event; perhaps here there is a functions
          //      events mismatch for sound-card and device ?
        }

        //.dP"Y8 888888    db     dP""b8 888888
        //`Ybo."   88     dPYb   dP   `" 88__
        // o.`Y8b   88    dP__Yb  Yb  "88 88""
        // 8bodP'   88   dP""""Yb  YboodP 888888

        // -----------------------------
        // -----    stage-added    -----
        // -----------------------------
        if(j["data"][0] == "stage-added") {
          ucout << "\n/--  STAGE_ADDED --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;

          // we add the stage to the stages inMemory array jsonObject
          stages.push_back(j["data"][1]);

          // print stages jsonObject array
          ucout << "stages:\n" << stages.dump(4) << std::endl;
        }

        // ------------------------------
        // -----    stage joined    -----
        // ------------------------------

        if(j["data"][0] == "stage-joined") {
          ucout << "\n/--  STAGE_JOINED --/\n" << std::endl;
          // put stage data into our stage jsonObject
          stage = j["data"][1];
          // print our stage
          ucout << "STAGE:\n" << stage.dump(4) << std::endl;

          // here we shall probably start a new tascar renderer
        }

        // ----------------------------
        // -----    stage-left    -----
        // ----------------------------

        if(j["data"][0] == "stage-left") {
          ucout << "\n/--  STAGE_LEFT --/\n" << std::endl;
          // we clear the stage jsonObject
          stage.clear();
          // print our stage
          ucout << "STAGE:\n" << stage.dump(4) << std::endl;
        }

        // 8b    d8 888888 8b    d8 88""Yb 888888 88""Yb .dP"Y8
        // 88b  d88 88__   88b  d88 88__dP 88__   88__dP `Ybo."
        // 88YbdP88 88""   88YbdP88 88""Yb 88""   88"Yb  o.`Y8b
        // 88 YY 88 888888 88 YY 88 88oodP 888888 88  Yb 8bodP'

        // -------------------------------
        // -----  stage-member-added -----
        // -------------------------------
        if(j["data"] == "stage-member-added") {
          ucout << "/-- STAGE_MEMBER_ADDED_EVENT --/" << std::endl;
          stage_members.push_back(j["data"][1]);

          // print stage_members jsonObject array
          ucout << "stage-members:\n" << stage_members.dump(4) << std::endl;
        }

        // ------------------------------------------
        // -----    stage-member-audio-added    -----
        // ------------------------------------------

        if(j["data"] == "stage-member-audio-added") {
          ucout << "/-- STAGE_MEMBER_AUDIO_ADDED --/" << std::endl;
        }

        // --------------------------------------
        // -----    stage-member-changed    -----
        // --------------------------------------

        if(j["data"] == "stage-member-changed") {
          ucout << "/-- STAGE_MEMBER_CHANGED --/" << std::endl;
        }

        // --------------------------------------
        // -----    stage-member-removed    -----
        // --------------------------------------
        if(j["data"] == "stage-member-removed") {
          ucout << "/-- STAGE_MEMBER_REMOVED --/" << std::endl;
        }

        // dP""b8 88""Yb  dP"Yb  88   88 88""Yb
        // dP   `" 88__dP dP   Yb 88   88 88__dP
        // Yb  "88 88"Yb  Yb   dP Y8   8P 88"""
        // YboodP 88  Yb  YbodP  `YbodP' 88

        // -----------------------------
        // -----    group-added    -----
        // -----------------------------
        if(j["data"][0] == "group-added") {
          ucout << "\n/--  GROUP_ADDED --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;

          // we add the group to groups inMemory array jsonObject
          groups.push_back(j["data"][1]);

          // print groups jsonObject array
          ucout << "groups:\n" << groups.dump(4) << std::endl;
        }

        // 888888 88""Yb    db     dP""b8 88  dP
        //  88   88__dP   dPYb   dP   `" 88odP
        //  88   88"Yb   dP__Yb  Yb      88"Yb
        //  88   88  Yb dP""""Yb  YboodP 88  Yb

        // ------------------------------------
        // -----    track-preset-added    -----
        // ------------------------------------
        if(j["data"][0] == "track-preset-added") {
          ucout << "\n/--  TRACK_PRESET_ADDED --/\n" << std::endl;
          ucout << j["data"][1].dump(4) << std::endl;

          // we add the group to groups inMemory array jsonObject
          track_presets.push_back(j["data"][1]);

          // print groups jsonObject array
          ucout << "track-presets:\n" << track_presets.dump(4) << std::endl;
        }
      }
      catch(const std::exception& e) {
        std::cerr << "std::exception: " << e.what() << std::endl;
      }
      catch(...) {
        std::cerr << "error parsing" << std::endl;
      }
    }
  });

  utility::string_t close_reason;
  wsclient.set_close_handler([&close_reason](websocket_close_status status,
                                             const utility::string_t& reason,
                                             const std::error_code& code) {
    ucout << " closing reason..." << reason << "\n";
    ucout << "connection closed, reason: " << reason
          << " close status: " << int(status) << " error code " << code
          << std::endl;
  });

  std::string macaddress(getmacaddr());

  std::cout << "MAC Address: " << macaddress << '\n';

  nlohmann::json token_json;

  nlohmann::json deviceJson;

  deviceJson["mac"] = macaddress;
  deviceJson["canVideo"] = false;
  deviceJson["canAudio"] = true;
  deviceJson["canOv"] = true;
  deviceJson["sendAudio"] = true;
  deviceJson["sendVideo"] = false;
  deviceJson["receiveAudio"] = true;
  deviceJson["receiveVideo"] = false;
  deviceJson["inputVideoDevices"] = nlohmann::json::array();
  deviceJson["inputAudioDevices"] = nlohmann::json::array();
  deviceJson["outputAudioDevices"] = nlohmann::json::array();
  deviceJson["soundCardIds"] = nlohmann::json::array();

  ucout << "Print device json string: \n" + deviceJson.dump() << std::endl;
  ucout << "PrettyPrint device json: \n" + deviceJson.dump(4) << std::endl;

  std::string body_str("{\"type\":0,\"data\":[\"token\",{\"token\":\"" + jwt +
                       "\"}]}");

  ucout << "token / device msg: " << body_str << std::endl;
  websocket_outgoing_message msg;
  msg.set_utf8_message(body_str);
  wsclient.send(msg).wait();
  receive_task.wait();

  // this part is never reached as receive_task.wait() is blocking the thread
  // until ov_client_digitalstage_t::stop_service() is called
  // this part is for reference only ! @Giso we can delete the while loop
  while(runservice) {
    std::cerr << "Error: not yet implemented." << std::endl;
    quitrequest_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
}

/*
 * Local Variables:
 * compile-command: "make -C .."
 * End:
 */
