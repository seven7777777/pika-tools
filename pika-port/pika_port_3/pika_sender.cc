#include <glog/logging.h>

#include "const.h"
#include "pika_sender.h"
#include "slash/include/xdebug.h"

PikaSender::PikaSender(int id, std::string ip, int64_t port, std::string password):
  id_(id),
  cli_(NULL),
  signal_(&keys_mutex_),
  ip_(ip),
  port_(port),
  password_(password),
  should_exit_(false),
  elements_(0) {
}

PikaSender::~PikaSender() {
}

int PikaSender::QueueSize() {
  slash::MutexLock l(&keys_mutex_);
  return keys_queue_.size();
}

void PikaSender::Stop() {
  should_exit_ = true;
  keys_mutex_.Lock();
  signal_.Signal();
  keys_mutex_.Unlock();
}

void PikaSender::ConnectRedis() {
  while (cli_ == NULL) {
    // Connect to redis
    cli_ = pink::NewRedisCli();
    cli_->set_connect_timeout(1000);
    slash::Status s = cli_->Connect(ip_, port_);
    if (!s.ok()) {
      delete cli_;
      cli_ = NULL;
      LOG(WARNING) << "PikaSender " << id_ << " Can not connect to "
        << ip_ << ":" << port_ << ", status: " << s.ToString();
      sleep(3);
      continue;
    } else {
      // Connect success
      LOG(INFO)  << "PikaSender " << id_ << " Connect to "
        << ip_ << ":" << port_ << " success";

      // Authentication
      if (!password_.empty()) {
        pink::RedisCmdArgsType argv, resp;
        std::string cmd;

        argv.push_back("AUTH");
        argv.push_back(password_);
        pink::SerializeRedisCommand(argv, &cmd);
        slash::Status s = cli_->Send(&cmd);

        if (s.ok()) {
          s = cli_->Recv(&resp);
          if (resp[0] == "OK") {
            LOG(INFO) << "PikaSender " << id_ << " Authentic success";
          } else {
            cli_->Close();
            LOG(WARNING) << "PikaSender " << id_ << " Invalid password";
            cli_ = NULL;
            should_exit_ = true;
            return;
          }
        } else {
          cli_->Close();
          LOG(INFO) << "PikaSender " << id_ << " Auth faild: " << s.ToString();
          cli_ = NULL;
          continue;
        }
      } else {
        // If forget to input password
        pink::RedisCmdArgsType argv, resp;
        std::string cmd;

        argv.push_back("PING");
        pink::SerializeRedisCommand(argv, &cmd);
        slash::Status s = cli_->Send(&cmd);

        if (s.ok()) {
          s = cli_->Recv(&resp);
          if (s.ok()) {
            if (resp[0] == "NOAUTH Authentication required.") {
              cli_->Close();
              LOG(WARNING) << "PikaSender " << id_ << " Authentication required";
              cli_ = NULL;
              should_exit_ = true;
              return;
            }
          } else {
            cli_->Close();
            LOG(INFO) << s.ToString();
            cli_ = NULL;
          }
        }
      }
    }
  }
}

void PikaSender::LoadKey(const std::string &key) {
  keys_mutex_.Lock();
  if (keys_queue_.size() < 100000) {
    keys_queue_.push(key);
    signal_.Signal();
    keys_mutex_.Unlock();
  } else {
    while (keys_queue_.size() > 100000 && !should_exit_) {
      signal_.TimedWait(100);
    }
    keys_queue_.push(key);
    signal_.Signal();
    keys_mutex_.Unlock();
  }
}

void PikaSender::SendCommand(std::string &command, const std::string &key) {
  bool alive = true;
  slash::Status s = cli_->Send(&command);
  if (s.ok()) {
    s = cli_->Recv(NULL);
    if (s.ok()) {
      return;
    } else {
      alive = false;
    }
  } else {
    alive = false;
  }

  if (!alive) {
    LOG(WARNING) << "PikaSender " << id_ << " Timeout disconnect, try to reconnect";
    elements_--;
    LoadKey(key);  // send command failed, reload the command
    cli_->Close();
    delete cli_;
    cli_ = NULL;
    ConnectRedis();
  }
}

void *PikaSender::ThreadMain() {
  LOG(INFO) << "Start PikaSender " << id_ << " Thread...";
  if (cli_ == NULL) {
    ConnectRedis();
  }

  while (!should_exit_ || QueueSize() != 0) {
    std::string command;

    keys_mutex_.Lock();
    while (keys_queue_.size() == 0 && !should_exit_) {
      signal_.TimedWait(200);
    }
    keys_mutex_.Unlock();
    if (QueueSize() == 0 && should_exit_) {
      return NULL;
    }

    keys_mutex_.Lock();
    std::string key = keys_queue_.front();
    elements_++;
    keys_queue_.pop();
    keys_mutex_.Unlock();
    SendCommand(key, key);
  }

  delete cli_;
  cli_ = NULL;
  LOG(INFO) << "PikaSender Thread " << id_ << " Complete";
  return NULL;
}

