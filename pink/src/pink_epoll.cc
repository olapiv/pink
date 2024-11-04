// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "pink/src/pink_epoll.h"

#ifdef __APPLE__
#else
#include <linux/version.h>
#endif
#include <fcntl.h>

#include "pink/include/pink_define.h"
#include "slash/include/xdebug.h"

namespace pink {

static const int kPinkMaxClients = 10240;

PinkEpoll::PinkEpoll(int queue_limit) : queue_limit_(queue_limit) {
#ifdef __APPLE__
  epfd_ = ::kqueue();
#else
#if defined(EPOLL_CLOEXEC)
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
#else
    epfd_ = epoll_create(1024);
#endif
#endif
  fcntl(epfd_, F_SETFD, fcntl(epfd_, F_GETFD) | FD_CLOEXEC);

  if (epfd_ < 0) {
#ifdef __APPLE__
    log_err("kqueue create fail");
#else
    log_err("epoll create fail");
#endif
    exit(1);
  }
  events_.resize(kPinkMaxClients);

  firedevent_ = reinterpret_cast<PinkFiredEvent*>(malloc(
      sizeof(PinkFiredEvent) * kPinkMaxClients));

  int fds[2];
  if (pipe(fds)) {
    exit(-1);
  }
  notify_receive_fd_ = fds[0];
  notify_send_fd_ = fds[1];

  fcntl(notify_receive_fd_, F_SETFD, fcntl(notify_receive_fd_, F_GETFD) | FD_CLOEXEC);
  fcntl(notify_send_fd_, F_SETFD, fcntl(notify_send_fd_, F_GETFD) | FD_CLOEXEC);

  PinkAddEvent(notify_receive_fd_, kRead);
}

PinkEpoll::~PinkEpoll() {
  free(firedevent_);
  close(epfd_);
}

int PinkEpoll::PinkAddEvent(const int fd, const int mask) {
#ifdef __APPLE__
  int cnt = 0;
  struct kevent change[2];
  if (mask & kRead) {
    EV_SET(change + cnt, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    ++cnt;
  }
  if (mask & kWrite) {
    EV_SET(change + cnt, fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    ++cnt;
  }
  return kevent(epfd_, change, cnt, nullptr, 0, nullptr);
#else
  struct epoll_event ee;
  ee.data.fd = fd;
  if (mask & kRead) {
    ee.events |= EPOLLIN;
  }
  if (mask & kWrite) {
    ee.events |= EPOLLOUT;
  }
  ee.events = mask;
  return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ee);
#endif
}

int PinkEpoll::PinkModEvent(const int fd, const int old_mask, const int mask) {
#ifdef __APPLE__
  int ret = PinkDelEvent(fd, kRead | kWrite);
  if (mask == 0) {
    return ret;
  }
  return PinkAddEvent(fd, mask);
#else
  struct epoll_event ee;
  ee.data.fd = fd;
  ee.events = 0;
  if ((old_mask | mask) & kRead) {
    ee.events |= EPOLLIN;
  }
  if ((old_mask | mask) & kWrite) {
    ee.events |= EPOLLOUT;
  }
  return epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ee);
#endif
}

int PinkEpoll::PinkDelEvent(const int fd, [[maybe_unused]] int mask) {
#ifdef __APPLE__
  int cnt = 0;
  struct kevent change[2];

  if (mask & kRead) {
    EV_SET(change + cnt, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    ++cnt;
  }
  if (mask & kWrite) {
    EV_SET(change + cnt, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ++cnt;
  }
  if (cnt == 0) {
    return -1;
  }
  return kevent(epfd_, change, cnt, nullptr, 0, nullptr);
#else
  /*
   * Kernel < 2.6.9 need a non null event point to EPOLL_CTL_DEL
   */
  struct epoll_event ee;
  ee.data.fd = fd;
  return epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &ee);
#endif
}

bool PinkEpoll::Register(const PinkItem& it, bool force) {
  bool success = false;
  notify_queue_protector_.Lock();
  if (force ||
      queue_limit_ == kUnlimitedQueue ||
      notify_queue_.size() < static_cast<size_t>(queue_limit_)) {
    notify_queue_.push(it);
    success = true;
  }
  notify_queue_protector_.Unlock();
  if (success) {
    write(notify_send_fd_, "", 1);
  }
  return success;
}

PinkItem PinkEpoll::notify_queue_pop() {
  PinkItem it;
  notify_queue_protector_.Lock();
  it = notify_queue_.front();
  notify_queue_.pop();
  notify_queue_protector_.Unlock();
  return it;
}

int PinkEpoll::PinkPoll(const int timeout) {
  int num_events = 0;
#ifdef __APPLE__
  struct timespec* p_timeout = nullptr;
  struct timespec s_timeout;
  if (timeout >= 0) {
    p_timeout = &s_timeout;
    s_timeout.tv_sec = timeout / 1000;
    s_timeout.tv_nsec = timeout % 1000 * 1000000;
  }
  num_events = ::kevent(epfd_, nullptr, 0, &events_[0], PINK_MAX_CLIENTS, p_timeout);
  if (num_events <= 0) {
    return 0;
  }
  for (int i = 0; i < num_events; i++) {
    PinkFiredEvent& ev = firedevent_[i];
    ev.fd = events_[i].ident;
    ev.mask = 0;

    if (events_[i].filter == EVFILT_READ) {
      ev.mask |= kRead;
    }

    if (events_[i].filter == EVFILT_WRITE) {
      ev.mask |= kWrite;
    }

    if (events_[i].flags & EV_ERROR) {
      ev.mask |= kError;
    }
  }
#else
  int retval = epoll_wait(epfd_, events_, PINK_MAX_CLIENTS, timeout);
  if (retval > 0) {
    num_events = retval;
    for (int i = 0; i < num_events; i++) {
      int mask = 0;
      firedevent_[i].fd = (events_ + i)->data.fd;

      if ((events_ + i)->events & EPOLLIN) {
        mask |= kRead;
      }
      if ((events_ + i)->events & EPOLLOUT) {
        mask |= kWrite;
      }
      if ((events_ + i)->events & EPOLLERR) {
        mask |= kErr;
      }
      if ((events_ + i)->events & EPOLLHUP) {
        mask |= kErr;
      }
      firedevent_[i].mask = mask;
    }
  }
#endif
  return num_events;
}

}  // namespace pink
