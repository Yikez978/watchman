/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

namespace watchman {
void InMemoryView::handleShouldRecrawl(unlocked_watchman_root* unlocked) {
  {
    auto info = unlocked->root->recrawlInfo.rlock();
    if (!info->shouldRecrawl) {
      return;
    }
  }

  auto root = unlocked->root;

  if (!root->inner.cancelled) {
    char *errmsg;
    auto info = root->recrawlInfo.wlock();

    info->shouldRecrawl = false;

    // be careful, this is a bit of a switcheroo
    w_root_teardown(root);
    if (!w_root_init(root, &errmsg)) {
      w_log(
          W_LOG_ERR,
          "failed to init root %s, cancelling watch: %s\n",
          root->root_path.c_str(),
          errmsg);
      // this should cause us to exit from the notify loop
      w_root_cancel(root);
    }
    info->recrawlCount++;
    // Tell the new view instance to start up
    root->inner.view->startThreads(root);
    w_pending_coll_ping(&pending_);
  }
}

// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work until after
// we have drained the inotify descriptor
void InMemoryView::notifyThread(unlocked_watchman_root* unlocked) {
  struct watchman_pending_collection pending;

  if (!watcher->start(unlocked->root)) {
    w_log(
        W_LOG_ERR,
        "failed to start root %s, cancelling watch: %s\n",
        unlocked->root->root_path.c_str(),
        unlocked->root->failure_reason.c_str());
    w_root_cancel(unlocked->root);
    return;
  }

  // signal that we're done here, so that we can start the
  // io thread after this point
  w_pending_coll_lock(&pending_);
  pending_.pinged = true;
  w_pending_coll_ping(&pending_);
  w_pending_coll_unlock(&pending_);

  while (!stopThreads_) {
    // big number because not all watchers can deal with
    // -1 meaning infinite wait at the moment
    if (watcher->waitNotify(86400)) {
      while (watcher->consumeNotify(unlocked->root, &pending)) {
        if (w_pending_coll_size(&pending) >= WATCHMAN_BATCH_LIMIT) {
          break;
        }
        if (!watcher->waitNotify(0)) {
          break;
        }
      }
      if (w_pending_coll_size(&pending) > 0) {
        w_pending_coll_lock(&pending_);
        w_pending_coll_append(&pending_, &pending);
        w_pending_coll_ping(&pending_);
        w_pending_coll_unlock(&pending_);
      }
    }
  }
  handleShouldRecrawl(unlocked);
}
}

/* vim:ts=2:sw=2:et:
 */
