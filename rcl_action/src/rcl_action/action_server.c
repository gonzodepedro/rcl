// Copyright 2018 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifdef __cplusplus
extern "C"
{
#endif

#include "rcl_action/action_server.h"

#include "rcl_action/default_qos.h"
#include "rcl_action/goal_handle.h"
#include "rcl_action/names.h"
#include "rcl_action/types.h"

#include "rcl/error_handling.h"
#include "rcl/rcl.h"
#include "rcl/time.h"

#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"

#include "rmw/rmw.h"

/// Internal rcl_action implementation struct.
typedef struct rcl_action_server_impl_t
{
  rcl_service_t goal_service;
  rcl_service_t cancel_service;
  rcl_service_t result_service;
  rcl_publisher_t feedback_publisher;
  rcl_publisher_t status_publisher;
  char * action_name;
  rcl_action_server_options_t options;
  // Array of goal handles
  rcl_action_goal_handle_t ** goal_handles;
  size_t num_goal_handles;
  // Clock
  rcl_clock_t clock;
} rcl_action_server_impl_t;

rcl_action_server_t
rcl_action_get_zero_initialized_server(void)
{
  static rcl_action_server_t null_action_server = {0};
  return null_action_server;
}

#define SERVICE_INIT(Type) \
  char * Type ## _service_name = NULL; \
  ret = rcl_action_get_ ## Type ## _service_name(action_name, allocator, &Type ## _service_name); \
  if (RCL_RET_OK != ret) { \
    RCL_SET_ERROR_MSG("failed to get " #Type " service name"); \
    if (RCL_RET_BAD_ALLOC == ret) { \
      ret = RCL_RET_BAD_ALLOC; \
    } else { \
      ret = RCL_RET_ERROR; \
    } \
    goto fail; \
  } \
  rcl_service_options_t Type ## _service_options = { \
    .qos = options->Type ## _service_qos, .allocator = allocator \
  }; \
  action_server->impl->Type ## _service = rcl_get_zero_initialized_service(); \
  ret = rcl_service_init( \
    &action_server->impl->Type ## _service, \
    node, \
    type_support->Type ## _service_type_support, \
    Type ## _service_name, \
    &Type ## _service_options); \
  allocator.deallocate(Type ## _service_name, allocator.state); \
  if (RCL_RET_OK != ret) { \
    if (RCL_RET_BAD_ALLOC == ret) { \
      ret = RCL_RET_BAD_ALLOC; \
    } else if (RCL_RET_SERVICE_NAME_INVALID == ret) { \
      ret = RCL_RET_ACTION_NAME_INVALID; \
    } else { \
      ret = RCL_RET_ERROR; \
    } \
    goto fail; \
  }

#define PUBLISHER_INIT(Type) \
  char * Type ## _topic_name = NULL; \
  ret = rcl_action_get_ ## Type ## _topic_name(action_name, allocator, &Type ## _topic_name); \
  if (RCL_RET_OK != ret) { \
    RCL_SET_ERROR_MSG("failed to get " #Type " topic name"); \
    if (RCL_RET_BAD_ALLOC == ret) { \
      ret = RCL_RET_BAD_ALLOC; \
    } else { \
      ret = RCL_RET_ERROR; \
    } \
    goto fail; \
  } \
  rcl_publisher_options_t Type ## _publisher_options = { \
    .qos = options->Type ## _topic_qos, .allocator = allocator \
  }; \
  action_server->impl->Type ## _publisher = rcl_get_zero_initialized_publisher(); \
  ret = rcl_publisher_init( \
    &action_server->impl->Type ## _publisher, \
    node, \
    type_support->Type ## _message_type_support, \
    Type ## _topic_name, \
    &Type ## _publisher_options); \
  allocator.deallocate(Type ## _topic_name, allocator.state); \
  if (RCL_RET_OK != ret) { \
    if (RCL_RET_BAD_ALLOC == ret) { \
      ret = RCL_RET_BAD_ALLOC; \
    } else if (RCL_RET_TOPIC_NAME_INVALID == ret) { \
      ret = RCL_RET_ACTION_NAME_INVALID; \
    } else { \
      ret = RCL_RET_ERROR; \
    } \
    goto fail; \
  }

rcl_ret_t
rcl_action_server_init(
  rcl_action_server_t * action_server,
  rcl_node_t * node,
  rcl_clock_t * clock,
  const rosidl_action_type_support_t * type_support,
  const char * action_name,
  const rcl_action_server_options_t * options)
{
  RCL_CHECK_ARGUMENT_FOR_NULL(action_server, RCL_RET_INVALID_ARGUMENT);
  if (!rcl_node_is_valid(node)) {
    return RCL_RET_NODE_INVALID;  // error already set
  }
  if (!rcl_clock_valid(clock)) {
    RCL_SET_ERROR_MSG("invalid clock");
    return RCL_RET_INVALID_ARGUMENT;
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(type_support, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(action_name, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(options, RCL_RET_INVALID_ARGUMENT);
  rcl_allocator_t allocator = options->allocator;
  RCL_CHECK_ALLOCATOR_WITH_MSG(&allocator, "invalid allocator", return RCL_RET_INVALID_ARGUMENT);

  RCUTILS_LOG_DEBUG_NAMED(
    ROS_PACKAGE_NAME, "Initializing action server for action name '%s'", action_name);
  if (action_server->impl) {
    RCL_SET_ERROR_MSG("action server already initialized, or memory was unintialized");
    return RCL_RET_ALREADY_INIT;
  }

  // Allocate for action server
  action_server->impl = (rcl_action_server_impl_t *)allocator.allocate(
    sizeof(rcl_action_server_impl_t), allocator.state);
  RCL_CHECK_FOR_NULL_WITH_MSG(
    action_server->impl, "allocating memory failed", return RCL_RET_BAD_ALLOC);

  rcl_ret_t ret = RCL_RET_OK;
  // Initialize services
  SERVICE_INIT(goal);
  SERVICE_INIT(cancel);
  SERVICE_INIT(result);

  // Initialize publishers
  PUBLISHER_INIT(feedback);
  PUBLISHER_INIT(status);

  // Copy clock
  action_server->impl->clock = *clock;

  // Copy action name
  action_server->impl->action_name = rcutils_strdup(action_name, allocator);
  if (NULL == action_server->impl->action_name) {
    ret = RCL_RET_BAD_ALLOC;
    goto fail;
  }

  // Copy options
  action_server->impl->options = *options;

  // Initialize goal handle array
  action_server->impl->goal_handles = NULL;
  action_server->impl->num_goal_handles = 0;

  return ret;
fail:
  {
    // Finalize any services/publishers that were initialized and deallocate action_server->impl
    rcl_ret_t ret_throwaway = rcl_action_server_fini(action_server, node);
    // Since there is already a failure, it is likely that finalize will error on one or more of
    // the action servers members
    (void)ret_throwaway;
  }
  return ret;
}

rcl_ret_t
rcl_action_server_fini(rcl_action_server_t * action_server, rcl_node_t * node)
{
  RCL_CHECK_ARGUMENT_FOR_NULL(action_server, RCL_RET_ACTION_SERVER_INVALID);
  if (!rcl_node_is_valid(node)) {
    return RCL_RET_NODE_INVALID;  // error already set
  }

  rcl_ret_t ret = RCL_RET_OK;
  if (action_server->impl) {
    // Finalize services
    if (rcl_service_fini(&action_server->impl->goal_service, node) != RCL_RET_OK) {
      ret = RCL_RET_ERROR;
    }
    if (rcl_service_fini(&action_server->impl->cancel_service, node) != RCL_RET_OK) {
      ret = RCL_RET_ERROR;
    }
    if (rcl_service_fini(&action_server->impl->result_service, node) != RCL_RET_OK) {
      ret = RCL_RET_ERROR;
    }
    // Finalize publishers
    if (rcl_publisher_fini(&action_server->impl->feedback_publisher, node) != RCL_RET_OK) {
      ret = RCL_RET_ERROR;
    }
    if (rcl_publisher_fini(&action_server->impl->status_publisher, node) != RCL_RET_OK) {
      ret = RCL_RET_ERROR;
    }
    // Deallocate action name
    rcl_allocator_t allocator = action_server->impl->options.allocator;
    if (action_server->impl->action_name) {
      allocator.deallocate(action_server->impl->action_name, allocator.state);
    }
    // Deallocate struct
    allocator.deallocate(action_server->impl, allocator.state);
    action_server->impl = NULL;
  }
  return ret;
}

rcl_action_server_options_t
rcl_action_server_get_default_options(void)
{
  // !!! MAKE SURE THAT CHANGES TO THESE DEFAULTS ARE REFLECTED IN THE HEADER DOC STRING
  static rcl_action_server_options_t default_options;
  default_options.goal_service_qos = rmw_qos_profile_services_default;
  default_options.cancel_service_qos = rmw_qos_profile_services_default;
  default_options.result_service_qos = rmw_qos_profile_services_default;
  default_options.feedback_topic_qos = rmw_qos_profile_default;
  default_options.status_topic_qos = rcl_action_qos_profile_status_default;
  default_options.allocator = rcl_get_default_allocator();
  default_options.result_timeout.nanoseconds = RCUTILS_S_TO_NS(15 * 60);  // 15 minutes
  return default_options;
}

#define TAKE_SERVICE_REQUEST(Type) \
  if (!rcl_action_server_is_valid(action_server)) { \
    return RCL_RET_ACTION_SERVER_INVALID;  /* error already set */ \
  } \
  RCL_CHECK_ARGUMENT_FOR_NULL(ros_ ## Type ## _request, RCL_RET_INVALID_ARGUMENT); \
  rmw_request_id_t request_header;  /* ignored */ \
  rcl_ret_t ret = rcl_take_request( \
    &action_server->impl->Type ## _service, &request_header, ros_ ## Type ## _request); \
  if (RCL_RET_OK != ret) { \
    if (RCL_RET_BAD_ALLOC == ret) { \
      return RCL_RET_BAD_ALLOC;  /* error already set */ \
    } \
    if (RCL_RET_SERVICE_TAKE_FAILED == ret) { \
      return RCL_RET_ACTION_SERVER_TAKE_FAILED; \
    } \
    return RCL_RET_ERROR;  /* error already set */ \
  } \
  return RCL_RET_OK; \

#define SEND_SERVICE_RESPONSE(Type) \
  if (!rcl_action_server_is_valid(action_server)) { \
    return RCL_RET_ACTION_SERVER_INVALID;  /* error already set */ \
  } \
  RCL_CHECK_ARGUMENT_FOR_NULL(ros_ ## Type ## _response, RCL_RET_INVALID_ARGUMENT); \
  rmw_request_id_t request_header;  /* ignored */ \
  rcl_ret_t ret = rcl_send_response( \
    &action_server->impl->Type ## _service, &request_header, ros_ ## Type ## _response); \
  if (RCL_RET_OK != ret) { \
    return RCL_RET_ERROR;  /* error already set */ \
  } \
  return RCL_RET_OK; \

rcl_ret_t
rcl_action_take_goal_request(
  const rcl_action_server_t * action_server,
  void * ros_goal_request)
{
  TAKE_SERVICE_REQUEST(goal);
}

rcl_ret_t
rcl_action_send_goal_response(
  const rcl_action_server_t * action_server,
  void * ros_goal_response)
{
  SEND_SERVICE_RESPONSE(goal);
}

// Implementation only
static int64_t
_goal_info_stamp_to_nanosec(const rcl_action_goal_info_t * goal_info)
{
  assert(goal_info);
  return RCUTILS_S_TO_NS(goal_info->stamp.sec) + (int64_t)goal_info->stamp.nanosec;
}

// Implementation only
static void
_nanosec_to_goal_info_stamp(const int64_t * nanosec, rcl_action_goal_info_t * goal_info)
{
  assert(nanosec);
  assert(goal_info);
  goal_info->stamp.sec = (int32_t)RCUTILS_NS_TO_S(*nanosec);
  goal_info->stamp.nanosec = *nanosec % RCUTILS_S_TO_NS(1);
}

rcl_action_goal_handle_t *
rcl_action_accept_new_goal(
  rcl_action_server_t * action_server,
  const rcl_action_goal_info_t * goal_info)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return NULL;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(goal_info, NULL);

  // Check if goal with same ID already exists
  if (rcl_action_server_goal_exists(action_server, goal_info)) {
    RCL_SET_ERROR_MSG("goal ID already exists");
    return NULL;
  }

  // Allocate space in the goal handle pointer array
  rcl_allocator_t allocator = action_server->impl->options.allocator;
  rcl_action_goal_handle_t ** goal_handles = action_server->impl->goal_handles;
  const size_t num_goal_handles = action_server->impl->num_goal_handles;
  // TODO(jacobperron): Don't allocate for every accepted goal handle,
  //                    instead double the memory allocated if needed.
  const size_t new_num_goal_handles = num_goal_handles + 1;
  void * tmp_ptr = allocator.reallocate(
    goal_handles, new_num_goal_handles * sizeof(rcl_action_goal_handle_t *), allocator.state);
  if (!tmp_ptr) {
    RCL_SET_ERROR_MSG("memory allocation failed for goal handle pointer");
    return NULL;
  }
  goal_handles = (rcl_action_goal_handle_t **)tmp_ptr;

  // Allocate space for a new goal handle
  tmp_ptr = allocator.allocate(sizeof(rcl_action_goal_handle_t), allocator.state);
  if (!tmp_ptr) {
    RCL_SET_ERROR_MSG("memory allocation failed for new goal handle");
    return NULL;
  }
  goal_handles[num_goal_handles] = (rcl_action_goal_handle_t *) tmp_ptr;

  // Re-stamp goal info with current time
  rcl_action_goal_info_t goal_info_stamp_now = rcl_action_get_zero_initialized_goal_info();
  goal_info_stamp_now = *goal_info;
  rcl_time_point_value_t now_time_point;
  rcl_ret_t ret = rcl_clock_get_now(&action_server->impl->clock, &now_time_point);
  if (RCL_RET_OK != ret) {
    return NULL;  // Error already set
  }
  _nanosec_to_goal_info_stamp(&now_time_point, &goal_info_stamp_now);

  // Create a new goal handle
  *goal_handles[num_goal_handles] = rcl_action_get_zero_initialized_goal_handle();
  ret = rcl_action_goal_handle_init(
    goal_handles[num_goal_handles], &goal_info_stamp_now, allocator);
  if (RCL_RET_OK != ret) {
    RCL_SET_ERROR_MSG("failed to initialize goal handle");
    return NULL;
  }

  action_server->impl->goal_handles = goal_handles;
  action_server->impl->num_goal_handles = new_num_goal_handles;
  return goal_handles[num_goal_handles];
}

rcl_ret_t
rcl_action_publish_feedback(
  const rcl_action_server_t * action_server,
  void * ros_feedback)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return RCL_RET_ACTION_SERVER_INVALID;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(ros_feedback, RCL_RET_INVALID_ARGUMENT);

  rcl_ret_t ret = rcl_publish(&action_server->impl->feedback_publisher, ros_feedback);
  if (RCL_RET_OK != ret) {
    return RCL_RET_ERROR;  // error already set
  }
  return RCL_RET_OK;
}

rcl_ret_t
rcl_action_get_goal_status_array(
  const rcl_action_server_t * action_server,
  rcl_action_goal_status_array_t * status_message)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return RCL_RET_ACTION_SERVER_INVALID;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(status_message, RCL_RET_INVALID_ARGUMENT);

  // If number of goals is zero, then we don't need to do any allocation
  size_t num_goals = action_server->impl->num_goal_handles;
  if (0 == num_goals) {
    status_message->msg.status_list.size = 0u;
    return RCL_RET_OK;
  }

  // Allocate status array
  rcl_allocator_t allocator = action_server->impl->options.allocator;
  rcl_ret_t ret = rcl_action_goal_status_array_init(status_message, num_goals, allocator);
  if (RCL_RET_OK != ret) {
    if (RCL_RET_BAD_ALLOC == ret) {
      return RCL_RET_BAD_ALLOC;
    }
    return RCL_RET_ERROR;
  }

  // Populate status array
  for (size_t i = 0; i < num_goals; ++i) {
    ret = rcl_action_goal_handle_get_info(
      action_server->impl->goal_handles[i], &status_message->msg.status_list.data[i].goal_info);
    if (RCL_RET_OK != ret) {
      ret = RCL_RET_ERROR;
      goto fail;
    }
    ret = rcl_action_goal_handle_get_status(
      action_server->impl->goal_handles[i], &status_message->msg.status_list.data[i].status);
    if (RCL_RET_OK != ret) {
      ret = RCL_RET_ERROR;
      goto fail;
    }
  }
  return RCL_RET_OK;
fail:
  {
    rcl_ret_t ret_throwaway = rcl_action_goal_status_array_fini(status_message);
    (void)ret_throwaway;
    return ret;
  }
}

rcl_ret_t
rcl_action_publish_status(
  const rcl_action_server_t * action_server,
  const void * status_message)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return RCL_RET_ACTION_SERVER_INVALID;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(status_message, RCL_RET_INVALID_ARGUMENT);

  rcl_ret_t ret = rcl_publish(&action_server->impl->status_publisher, status_message);
  if (RCL_RET_OK != ret) {
    return RCL_RET_ERROR;  // error already set
  }
  return RCL_RET_OK;
}

rcl_ret_t
rcl_action_take_result_request(
  const rcl_action_server_t * action_server,
  void * ros_result_request)
{
  TAKE_SERVICE_REQUEST(result);
}

rcl_ret_t
rcl_action_send_result_response(
  const rcl_action_server_t * action_server,
  void * ros_result_response)
{
  SEND_SERVICE_RESPONSE(result);
}

rcl_ret_t
rcl_action_clear_expired_goals(
  const rcl_action_server_t * action_server,
  size_t * num_expired)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return RCL_RET_ACTION_SERVER_INVALID;
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(num_expired, RCL_RET_INVALID_ARGUMENT);

  // Get current time (nanosec)
  int64_t current_time;
  rcl_ret_t ret = rcl_clock_get_now(&action_server->impl->clock, &current_time);
  if (RCL_RET_OK != ret) {
    return RCL_RET_ERROR;
  }

  // Used for shrinking goal handle array
  rcl_allocator_t allocator = action_server->impl->options.allocator;

  *num_expired = 0u;
  rcl_ret_t ret_final = RCL_RET_OK;
  const int64_t timeout = (int64_t)action_server->impl->options.result_timeout.nanoseconds;
  rcl_action_goal_handle_t * goal_handle;
  rcl_action_goal_info_t goal_info;
  int64_t goal_time;
  size_t num_goal_handles = action_server->impl->num_goal_handles;
  for (size_t i = 0; i < num_goal_handles; ++i) {
    goal_handle = action_server->impl->goal_handles[i];
    // Expiration only applys to terminated goals
    if (rcl_action_goal_handle_is_active(goal_handle)) {
      continue;
    }
    ret = rcl_action_goal_handle_get_info(goal_handle, &goal_info);
    if (RCL_RET_OK != ret) {
      ret_final = RCL_RET_ERROR;
      continue;
    }
    goal_time = _goal_info_stamp_to_nanosec(&goal_info);
    assert(current_time > goal_time);
    if ((current_time - goal_time) > timeout) {
      // Stop tracking goal handle
      ret = rcl_action_goal_handle_fini(goal_handle);
      if (RCL_RET_OK != ret) {
        ret_final = RCL_RET_ERROR;
      }
      // Fill in any gaps left in the array with pointers from the end
      action_server->impl->goal_handles[i] = action_server->impl->goal_handles[num_goal_handles];
      action_server->impl->goal_handles[num_goal_handles--] = NULL;

      ++(*num_expired);
    }
  }

  // Shrink goal handle array if some goals expired
  if (*num_expired > 0u) {
    if (0 == num_goal_handles) {
      allocator.deallocate(action_server->impl->goal_handles, allocator.state);
    } else {
      void * tmp_ptr = allocator.reallocate(
        action_server->impl->goal_handles, num_goal_handles, allocator.state);
      if (!tmp_ptr) {
        RCL_SET_ERROR_MSG("failed to shrink size of goal handle array");
        ret_final = RCL_RET_ERROR;
      } else {
        action_server->impl->goal_handles = (rcl_action_goal_handle_t **)tmp_ptr;
        action_server->impl->num_goal_handles = num_goal_handles;
      }
    }
  }
  return ret_final;
}

rcl_ret_t
rcl_action_take_cancel_request(
  const rcl_action_server_t * action_server,
  void * ros_cancel_request)
{
  TAKE_SERVICE_REQUEST(cancel);
}

rcl_ret_t
rcl_action_process_cancel_request(
  const rcl_action_server_t * action_server,
  const rcl_action_cancel_request_t * cancel_request,
  rcl_action_cancel_response_t * cancel_response)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return RCL_RET_ACTION_SERVER_INVALID;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(cancel_request, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(cancel_response, RCL_RET_INVALID_ARGUMENT);

  rcl_allocator_t allocator = action_server->impl->options.allocator;
  const size_t total_num_goals = action_server->impl->num_goal_handles;

  // Storage for pointers to active goals handles that will be transitioned to canceling
  // Note, we need heap allocation for MSVC support
  rcl_action_goal_handle_t ** goal_handles_to_cancel =
    (rcl_action_goal_handle_t **)allocator.allocate(
    sizeof(rcl_action_goal_handle_t *) * total_num_goals, allocator.state);
  if (!goal_handles_to_cancel) {
    RCL_SET_ERROR_MSG("allocation failed for temporary goal handle array");
    return RCL_RET_BAD_ALLOC;
  }
  size_t num_goals_to_cancel = 0u;

  // Request data
  const rcl_action_goal_info_t * request_goal_info = &cancel_request->goal_info;
  const uint8_t * request_uuid = request_goal_info->uuid;
  int64_t request_nanosec = _goal_info_stamp_to_nanosec(request_goal_info);

  rcl_ret_t ret_final = RCL_RET_OK;
  // Determine how many goals should transition to canceling
  if (!uuidcmpzero(request_uuid) && (0u == request_nanosec)) {
    // UUID is not zero and timestamp is zero; cancel exactly one goal (if it exists)
    rcl_action_goal_info_t goal_info = rcl_action_get_zero_initialized_goal_info();
    rcl_action_goal_handle_t * goal_handle;
    for (size_t i = 0; i < total_num_goals; ++i) {
      goal_handle = action_server->impl->goal_handles[i];
      rcl_ret_t ret = rcl_action_goal_handle_get_info(goal_handle, &goal_info);
      if (RCL_RET_OK != ret) {
        ret_final = RCL_RET_ERROR;
        continue;
      }

      if (uuidcmp(request_uuid, goal_info.uuid)) {
        if (rcl_action_goal_handle_is_cancelable(goal_handle)) {
          goal_handles_to_cancel[num_goals_to_cancel++] = goal_handle;
        }
        break;
      }
    }
  } else {
    if (uuidcmpzero(request_uuid) && (0u == request_nanosec)) {
      // UUID and timestamp are both zero; cancel all goals
      // Set timestamp to max to cancel all active goals in the following for-loop
      request_nanosec = INT64_MAX;
    }

    // Cancel all active goals at or before the timestamp
    // Also cancel any goal matching the UUID in the cancel request
    rcl_action_goal_info_t goal_info = rcl_action_get_zero_initialized_goal_info();
    rcl_action_goal_handle_t * goal_handle;
    for (size_t i = 0; i < total_num_goals; ++i) {
      goal_handle = action_server->impl->goal_handles[i];
      rcl_ret_t ret = rcl_action_goal_handle_get_info(goal_handle, &goal_info);
      if (RCL_RET_OK != ret) {
        ret_final = RCL_RET_ERROR;
        continue;
      }

      const int64_t goal_nanosec = _goal_info_stamp_to_nanosec(&goal_info);
      if (rcl_action_goal_handle_is_cancelable(goal_handle) &&
        ((goal_nanosec <= request_nanosec) || uuidcmp(request_uuid, goal_info.uuid)))
      {
        goal_handles_to_cancel[num_goals_to_cancel++] = goal_handle;
      }
    }
  }

  if (0u == num_goals_to_cancel) {
    cancel_response->msg.goals_canceling.data = NULL;
    cancel_response->msg.goals_canceling.size = 0u;
    goto cleanup;
  }

  // Allocate space in response
  rcl_ret_t ret = rcl_action_cancel_response_init(
    cancel_response, num_goals_to_cancel, allocator);
  if (RCL_RET_OK != ret) {
    if (RCL_RET_BAD_ALLOC == ret) {
      ret_final = RCL_RET_BAD_ALLOC;  // error already set
    }
    ret_final = RCL_RET_ERROR;  // error already set
    goto cleanup;
  }

  // Transition goals to canceling and add to response
  rcl_action_goal_handle_t * goal_handle;
  for (size_t i = 0; i < num_goals_to_cancel; ++i) {
    goal_handle = goal_handles_to_cancel[i];
    ret = rcl_action_update_goal_state(goal_handle, GOAL_EVENT_CANCEL);
    if (RCL_RET_OK == ret) {
      ret = rcl_action_goal_handle_get_info(
        goal_handle, &cancel_response->msg.goals_canceling.data[i]);
    }
    if (RCL_RET_OK != ret) {
      ret_final = RCL_RET_ERROR;  // error already set
    }
  }
cleanup:
  allocator.deallocate(goal_handles_to_cancel, allocator.state);
  return ret_final;
}

rcl_ret_t
rcl_action_send_cancel_response(
  const rcl_action_server_t * action_server,
  void * ros_cancel_response)
{
  SEND_SERVICE_RESPONSE(cancel);
}

const char *
rcl_action_server_get_action_name(const rcl_action_server_t * action_server)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return NULL;  // error already set
  }
  return action_server->impl->action_name;
}

const rcl_action_server_options_t *
rcl_action_server_get_options(const rcl_action_server_t * action_server)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return NULL;  // error already set
  }
  return &action_server->impl->options;
}

rcl_ret_t
rcl_action_server_get_goal_handles(
  const rcl_action_server_t * action_server,
  rcl_action_goal_handle_t *** goal_handles,
  size_t * num_goals)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return RCL_RET_ACTION_SERVER_INVALID;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(goal_handles, RCL_RET_INVALID_ARGUMENT);
  RCL_CHECK_ARGUMENT_FOR_NULL(num_goals, RCL_RET_INVALID_ARGUMENT);
  *goal_handles = action_server->impl->goal_handles;
  *num_goals = action_server->impl->num_goal_handles;
  return RCL_RET_OK;
}

bool
rcl_action_server_goal_exists(
  const rcl_action_server_t * action_server,
  const rcl_action_goal_info_t * goal_info)
{
  if (!rcl_action_server_is_valid(action_server)) {
    return false;  // error already set
  }
  RCL_CHECK_ARGUMENT_FOR_NULL(goal_info, false);

  rcl_action_goal_info_t gh_goal_info = rcl_action_get_zero_initialized_goal_info();
  rcl_ret_t ret;
  for (size_t i = 0; i < action_server->impl->num_goal_handles; ++i) {
    ret = rcl_action_goal_handle_get_info(action_server->impl->goal_handles[i], &gh_goal_info);
    if (RCL_RET_OK != ret) {
      RCL_SET_ERROR_MSG("failed to get info for goal handle");
      return false;
    }
    // Compare UUIDs
    if (uuidcmp(gh_goal_info.uuid, goal_info->uuid)) {
      return true;
    }
  }
  return false;
}

bool
rcl_action_server_is_valid(const rcl_action_server_t * action_server)
{
  RCL_CHECK_FOR_NULL_WITH_MSG(action_server, "action server pointer is invalid", return false);
  RCL_CHECK_FOR_NULL_WITH_MSG(
    action_server->impl, "action server implementation is invalid", return false);
  if (!rcl_service_is_valid(&action_server->impl->goal_service)) {
    RCL_SET_ERROR_MSG("goal service is invalid");
    return false;
  }
  if (!rcl_service_is_valid(&action_server->impl->cancel_service)) {
    RCL_SET_ERROR_MSG("cancel service is invalid");
    return false;
  }
  if (!rcl_service_is_valid(&action_server->impl->result_service)) {
    RCL_SET_ERROR_MSG("result service is invalid");
    return false;
  }
  if (!rcl_publisher_is_valid(&action_server->impl->feedback_publisher)) {
    RCL_SET_ERROR_MSG("feedback publisher is invalid");
    return false;
  }
  if (!rcl_publisher_is_valid(&action_server->impl->status_publisher)) {
    RCL_SET_ERROR_MSG("status publisher is invalid");
    return false;
  }
  return true;
}

#ifdef __cplusplus
}
#endif
