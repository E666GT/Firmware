/****************************************************************************
 *
 *   Copyright (c) 2013-2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mc_att_control_main.cpp
 * Multicopter attitude controller.
 *
 * @author Lorenz Meier		<lorenz@px4.io>
 * @author Anton Babushkin	<anton.babushkin@me.com>
 * @author Sander Smeets	<sander@droneslab.com>
 * @author Matthias Grob	<maetugr@gmail.com>
 * @author Beat Küng		<beat-kueng@gmx.net>
 *
 */
//----CYW_MC--HEAD-----
#include <uORB/topics/vehicle_status.h>
//--------------------------

#include "mc_att_control.hpp"

#include <conversion/rotation.h>
#include <drivers/drv_hrt.h>
#include <lib/ecl/geo/geo.h>
#include <circuit_breaker/circuit_breaker.h>
#include <mathlib/math/Limits.hpp>
#include <mathlib/math/Functions.hpp>

#define TPA_RATE_LOWER_LIMIT 0.05f

#define AXIS_INDEX_ROLL 0
#define AXIS_INDEX_PITCH 1
#define AXIS_INDEX_YAW 2
#define AXIS_COUNT 3

using namespace matrix;


int MulticopterAttitudeControl::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
This implements the multicopter attitude and rate controller. It takes attitude
setpoints (`vehicle_attitude_setpoint`) or rate setpoints (in acro mode
via `manual_control_setpoint` topic) as inputs and outputs actuator control messages.

The controller has two loops: a P loop for angular error and a PID loop for angular rate error.

Publication documenting the implemented Quaternion Attitude Control:
Nonlinear Quadrocopter Attitude Control (2013)
by Dario Brescianini, Markus Hehn and Raffaello D'Andrea
Institute for Dynamic Systems and Control (IDSC), ETH Zurich

https://www.research-collection.ethz.ch/bitstream/handle/20.500.11850/154099/eth-7387-01.pdf

### Implementation
To reduce control latency, the module directly polls on the gyro topic published by the IMU driver.

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("mc_att_control", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}

MulticopterAttitudeControl::MulticopterAttitudeControl() :
	ModuleParams(nullptr),
	_loop_perf(perf_alloc(PC_ELAPSED, "mc_att_control"))
{
	for (uint8_t i = 0; i < MAX_GYRO_COUNT; i++) {
		_sensor_gyro_sub[i] = -1;
	}

	_vehicle_status.is_rotary_wing = true;

	/* initialize quaternions in messages to be valid */
	_v_att.q[0] = 1.f;
	_v_att_sp.q_d[0] = 1.f;

	_rates_prev.zero();
	_rates_prev_filtered.zero();
	_rates_sp.zero();
	_rates_int.zero();
	_thrust_sp = 0.0f;
	_att_control.zero();

	/* initialize thermal corrections as we might not immediately get a topic update (only non-zero values) */
	for (unsigned i = 0; i < 3; i++) {
		// used scale factors to unity
		_sensor_correction.gyro_scale_0[i] = 1.0f;
		_sensor_correction.gyro_scale_1[i] = 1.0f;
		_sensor_correction.gyro_scale_2[i] = 1.0f;
	}

	parameters_updated();
}

void
MulticopterAttitudeControl::parameters_updated()
{
	/* Store some of the parameters in a more convenient way & precompute often-used values */

	/* roll gains */
	_attitude_p(0) = _roll_p.get();
	_rate_p(0) = _roll_rate_p.get();
	_rate_i(0) = _roll_rate_i.get();
	_rate_int_lim(0) = _roll_rate_integ_lim.get();
	_rate_d(0) = _roll_rate_d.get();
	_rate_ff(0) = _roll_rate_ff.get();

	/* pitch gains */
	_attitude_p(1) = _pitch_p.get();
	_rate_p(1) = _pitch_rate_p.get();
	_rate_i(1) = _pitch_rate_i.get();
	_rate_int_lim(1) = _pitch_rate_integ_lim.get();
	_rate_d(1) = _pitch_rate_d.get();
	_rate_ff(1) = _pitch_rate_ff.get();

	/* yaw gains */
	_attitude_p(2) = _yaw_p.get();
	_rate_p(2) = _yaw_rate_p.get();
	_rate_i(2) = _yaw_rate_i.get();
	_rate_int_lim(2) = _yaw_rate_integ_lim.get();
	_rate_d(2) = _yaw_rate_d.get();
	_rate_ff(2) = _yaw_rate_ff.get();

	if (fabsf(_lp_filters_d.get_cutoff_freq() - _d_term_cutoff_freq.get()) > 0.01f) {
		_lp_filters_d.set_cutoff_frequency(_loop_update_rate_hz, _d_term_cutoff_freq.get());
		_lp_filters_d.reset(_rates_prev);
	}

	/* angular rate limits */
	_mc_rate_max(0) = math::radians(_roll_rate_max.get());
	_mc_rate_max(1) = math::radians(_pitch_rate_max.get());
	_mc_rate_max(2) = math::radians(_yaw_rate_max.get());

	/* auto angular rate limits */
	_auto_rate_max(0) = math::radians(_roll_rate_max.get());
	_auto_rate_max(1) = math::radians(_pitch_rate_max.get());
	_auto_rate_max(2) = math::radians(_yaw_auto_max.get());

	/* manual rate control acro mode rate limits and expo */
	_acro_rate_max(0) = math::radians(_acro_roll_max.get());
	_acro_rate_max(1) = math::radians(_acro_pitch_max.get());
	_acro_rate_max(2) = math::radians(_acro_yaw_max.get());

	_man_tilt_max = math::radians(_man_tilt_max_deg.get());

	_actuators_0_circuit_breaker_enabled = circuit_breaker_enabled("CBRK_RATE_CTRL", CBRK_RATE_CTRL_KEY);

	/* get transformation matrix from sensor/board to body frame */
	_board_rotation = get_rot_matrix((enum Rotation)_board_rotation_param.get());

	/* fine tune the rotation */
	Dcmf board_rotation_offset(Eulerf(
			M_DEG_TO_RAD_F * _board_offset_x.get(),
			M_DEG_TO_RAD_F * _board_offset_y.get(),
			M_DEG_TO_RAD_F * _board_offset_z.get()));
	_board_rotation = board_rotation_offset * _board_rotation;
}

void
MulticopterAttitudeControl::parameter_update_poll()
{
	bool updated;

	/* Check if parameters have changed */
	orb_check(_params_sub, &updated);

	if (updated) {
		struct parameter_update_s param_update;
		orb_copy(ORB_ID(parameter_update), _params_sub, &param_update);
		updateParams();
		parameters_updated();
	}
}

void
MulticopterAttitudeControl::vehicle_control_mode_poll()
{
	bool updated;

	/* Check if vehicle control mode has changed */
	orb_check(_v_control_mode_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_control_mode), _v_control_mode_sub, &_v_control_mode);
	}
}

bool
MulticopterAttitudeControl::vehicle_manual_poll()
{
	bool updated;

	/* get pilots inputs */
	orb_check(_manual_control_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(manual_control_setpoint), _manual_control_sp_sub, &_manual_control_sp);
		return true;
	}
	return false;
}

void
MulticopterAttitudeControl::vehicle_attitude_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_att_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_attitude_setpoint), _v_att_sp_sub, &_v_att_sp);
	}
}

bool
MulticopterAttitudeControl::vehicle_rates_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_rates_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_rates_setpoint), _v_rates_sp_sub, &_v_rates_sp);
		return true;
	}
	return false;
}

void
MulticopterAttitudeControl::vehicle_status_poll()
{
	/* check if there is new status information */
	bool vehicle_status_updated;
	orb_check(_vehicle_status_sub, &vehicle_status_updated);

	if (vehicle_status_updated) {
		orb_copy(ORB_ID(vehicle_status), _vehicle_status_sub, &_vehicle_status);

		/* set correct uORB ID, depending on if vehicle is VTOL or not */
		if (_actuators_id == nullptr) {
			if (_vehicle_status.is_vtol) {
				_actuators_id = ORB_ID(actuator_controls_virtual_mc);

			} else {
				_actuators_id = ORB_ID(actuator_controls_0);
			}
		}
	}
}

void
MulticopterAttitudeControl::vehicle_motor_limits_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_motor_limits_sub, &updated);

	if (updated) {
		multirotor_motor_limits_s motor_limits = {};
		orb_copy(ORB_ID(multirotor_motor_limits), _motor_limits_sub, &motor_limits);

		_saturation_status.value = motor_limits.saturation_status;
	}
}

void
MulticopterAttitudeControl::battery_status_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_battery_status_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(battery_status), _battery_status_sub, &_battery_status);
	}
}

bool
MulticopterAttitudeControl::vehicle_attitude_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_v_att_sub, &updated);

	if (updated) {
		uint8_t prev_quat_reset_counter = _v_att.quat_reset_counter;

		orb_copy(ORB_ID(vehicle_attitude), _v_att_sub, &_v_att);

		// Check for a heading reset
		if (prev_quat_reset_counter != _v_att.quat_reset_counter) {
			// we only extract the heading change from the delta quaternion
			_man_yaw_sp += Eulerf(Quatf(_v_att.delta_q_reset)).psi();
		}
		return true;
	}
	return false;
}

void
MulticopterAttitudeControl::sensor_correction_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_sensor_correction_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(sensor_correction), _sensor_correction_sub, &_sensor_correction);
	}

	/* update the latest gyro selection */
	if (_sensor_correction.selected_gyro_instance < _gyro_count) {
		_selected_gyro = _sensor_correction.selected_gyro_instance;
	}
}

void
MulticopterAttitudeControl::sensor_bias_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_sensor_bias_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(sensor_bias), _sensor_bias_sub, &_sensor_bias);
	}

}

void
MulticopterAttitudeControl::vehicle_land_detected_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_vehicle_land_detected_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_land_detected), _vehicle_land_detected_sub, &_vehicle_land_detected);
	}

}

void
MulticopterAttitudeControl::landing_gear_state_poll()
{
	bool updated;
	orb_check(_landing_gear_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(landing_gear), _landing_gear_sub, &_landing_gear);
	}
}

float
MulticopterAttitudeControl::throttle_curve(float throttle_stick_input)
{
	// throttle_stick_input is in range [0, 1]
	switch (_throttle_curve.get()) {
	case 1: // no rescaling to hover throttle
		return _man_throttle_min.get() + throttle_stick_input * (_throttle_max.get() - _man_throttle_min.get());

	default: // 0 or other: rescale to hover throttle at 0.5 stick
		if (throttle_stick_input < 0.5f) {
			return (_throttle_hover.get() - _man_throttle_min.get()) / 0.5f * throttle_stick_input + _man_throttle_min.get();

		} else {
			return (_throttle_max.get() - _throttle_hover.get()) / 0.5f * (throttle_stick_input - 1.0f) + _throttle_max.get();
		}
	}
}

float
MulticopterAttitudeControl::get_landing_gear_state()
{
	// Only switch the landing gear up if we are not landed and if
	// the user switched from gear down to gear up.
	// If the user had the switch in the gear up position and took off ignore it
	// until he toggles the switch to avoid retracting the gear immediately on takeoff.
	if (_vehicle_land_detected.landed) {
		_gear_state_initialized = false;
	}
	float landing_gear = landing_gear_s::GEAR_DOWN; // default to down
	if (_manual_control_sp.gear_switch == manual_control_setpoint_s::SWITCH_POS_ON && _gear_state_initialized) {
		landing_gear = landing_gear_s::GEAR_UP;

	} else if (_manual_control_sp.gear_switch == manual_control_setpoint_s::SWITCH_POS_OFF) {
		// Switching the gear off does put it into a safe defined state
		_gear_state_initialized = true;
	}

	return landing_gear;
}

void
MulticopterAttitudeControl::generate_attitude_setpoint(float dt, bool reset_yaw_sp)
{
//	vehicle_attitude_setpoint_s attitude_setpoint{};
//	landing_gear_s landing_gear{};
//	const float yaw = Eulerf(Quatf(_v_att.q)).psi();

	/* reset yaw setpoint to current position if needed */
//	if (reset_yaw_sp) {
//		_man_yaw_sp = yaw;

//	} else if (_manual_control_sp.z > 0.05f) {

//		const float yaw_rate = math::radians(_yaw_rate_scaling.get());
//		attitude_setpoint.yaw_sp_move_rate = _manual_control_sp.r * yaw_rate;
//		_man_yaw_sp = wrap_pi(_man_yaw_sp + attitude_setpoint.yaw_sp_move_rate * dt);
//	}

	/*
	 * Input mapping for roll & pitch setpoints
	 * ----------------------------------------
	 * We control the following 2 angles:
	 * - tilt angle, given by sqrt(x*x + y*y)
	 * - the direction of the maximum tilt in the XY-plane, which also defines the direction of the motion
	 *
	 * This allows a simple limitation of the tilt angle, the vehicle flies towards the direction that the stick
	 * points to, and changes of the stick input are linear.
	 */
//	const float x = _manual_control_sp.x * _man_tilt_max;
//	const float y = _manual_control_sp.y * _man_tilt_max;

	// we want to fly towards the direction of (x, y), so we use a perpendicular axis angle vector in the XY-plane
//	Vector2f v = Vector2f(y, -x);
//	float v_norm = v.norm(); // the norm of v defines the tilt angle

//	if (v_norm > _man_tilt_max) { // limit to the configured maximum tilt angle
//		v *= _man_tilt_max / v_norm;
//	}

//	Quatf q_sp_rpy = AxisAnglef(v(0), v(1), 0.f);
//	Eulerf euler_sp = q_sp_rpy;
//	attitude_setpoint.roll_body = euler_sp(0);
//	attitude_setpoint.pitch_body = euler_sp(1);
	// The axis angle can change the yaw as well (noticeable at higher tilt angles).
	// This is the formula by how much the yaw changes:
	//   let a := tilt angle, b := atan(y/x) (direction of maximum tilt)
	//   yaw = atan(-2 * sin(b) * cos(b) * sin^2(a/2) / (1 - 2 * cos^2(b) * sin^2(a/2))).
//	attitude_setpoint.yaw_body = _man_yaw_sp + euler_sp(2);

	/* modify roll/pitch only if we're a VTOL */
//	if (_vehicle_status.is_vtol) {
//		// Construct attitude setpoint rotation matrix. Modify the setpoints for roll
//		// and pitch such that they reflect the user's intention even if a large yaw error
//		// (yaw_sp - yaw) is present. In the presence of a yaw error constructing a rotation matrix
//		// from the pure euler angle setpoints will lead to unexpected attitude behaviour from
//		// the user's view as the euler angle sequence uses the  yaw setpoint and not the current
//		// heading of the vehicle.
//		// However there's also a coupling effect that causes oscillations for fast roll/pitch changes
//		// at higher tilt angles, so we want to avoid using this on multicopters.
//		// The effect of that can be seen with:
//		// - roll/pitch into one direction, keep it fixed (at high angle)
//		// - apply a fast yaw rotation
//		// - look at the roll and pitch angles: they should stay pretty much the same as when not yawing

//		// calculate our current yaw error
//		float yaw_error = wrap_pi(attitude_setpoint.yaw_body - yaw);

//		// compute the vector obtained by rotating a z unit vector by the rotation
//		// given by the roll and pitch commands of the user
//		Vector3f zB = {0.0f, 0.0f, 1.0f};
//		Dcmf R_sp_roll_pitch = Eulerf(attitude_setpoint.roll_body, attitude_setpoint.pitch_body, 0.0f);
//		Vector3f z_roll_pitch_sp = R_sp_roll_pitch * zB;

//		// transform the vector into a new frame which is rotated around the z axis
//		// by the current yaw error. this vector defines the desired tilt when we look
//		// into the direction of the desired heading
//		Dcmf R_yaw_correction = Eulerf(0.0f, 0.0f, -yaw_error);
//		z_roll_pitch_sp = R_yaw_correction * z_roll_pitch_sp;

//		// use the formula z_roll_pitch_sp = R_tilt * [0;0;1]
//		// R_tilt is computed from_euler; only true if cos(roll) not equal zero
//		// -> valid if roll is not +-pi/2;
//		attitude_setpoint.roll_body = -asinf(z_roll_pitch_sp(1));
//		attitude_setpoint.pitch_body = atan2f(z_roll_pitch_sp(0), z_roll_pitch_sp(2));
//	}

	/* copy quaternion setpoint to attitude setpoint topic */
//	Quatf q_sp = Eulerf(attitude_setpoint.roll_body, attitude_setpoint.pitch_body, attitude_setpoint.yaw_body);
//	q_sp.copyTo(attitude_setpoint.q_d);
//	attitude_setpoint.q_d_valid = true;

//	attitude_setpoint.thrust_body[2] = -throttle_curve(_manual_control_sp.z);

    _landing_gear.landing_gear = get_landing_gear_state();

//	attitude_setpoint.timestamp = landing_gear.timestamp = hrt_absolute_time();
//	orb_publish_auto(ORB_ID(vehicle_attitude_setpoint), &_vehicle_attitude_setpoint_pub, &attitude_setpoint, nullptr, ORB_PRIO_DEFAULT);
//	orb_publish_auto(ORB_ID(landing_gear), &_landing_gear_pub, &attitude_setpoint, nullptr, ORB_PRIO_DEFAULT);
}

/**
 * Attitude controller.
 * Input: 'vehicle_attitude_setpoint' topics (depending on mode)
 * Output: '_rates_sp' vector, '_thrust_sp'
 */
void
MulticopterAttitudeControl::control_attitude()
{
	vehicle_attitude_setpoint_poll();

//	// physical thrust axis is the negative of body z axis
//	_thrust_sp = -_v_att_sp.thrust_body[2];

//	/* prepare yaw weight from the ratio between roll/pitch and yaw gains */
//	Vector3f attitude_gain = _attitude_p;
//	const float roll_pitch_gain = (attitude_gain(0) + attitude_gain(1)) / 2.f;
//	const float yaw_w = math::constrain(attitude_gain(2) / roll_pitch_gain, 0.f, 1.f);
//	attitude_gain(2) = roll_pitch_gain;

//	/* get estimated and desired vehicle attitude */
//	Quatf q(_v_att.q);
//	Quatf qd(_v_att_sp.q_d);

//	/* ensure input quaternions are exactly normalized because acosf(1.00001) == NaN */
//	q.normalize();
//	qd.normalize();

//	/* calculate reduced desired attitude neglecting vehicle's yaw to prioritize roll and pitch */
//	Vector3f e_z = q.dcm_z();
//	Vector3f e_z_d = qd.dcm_z();
//	Quatf qd_red(e_z, e_z_d);

//	if (abs(qd_red(1)) > (1.f - 1e-5f) || abs(qd_red(2)) > (1.f - 1e-5f)) {
//		/* In the infinitesimal corner case where the vehicle and thrust have the completely opposite direction,
//		 * full attitude control anyways generates no yaw input and directly takes the combination of
//		 * roll and pitch leading to the correct desired yaw. Ignoring this case would still be totally safe and stable. */
//		qd_red = qd;

//	} else {
//		/* transform rotation from current to desired thrust vector into a world frame reduced desired attitude */
//		qd_red *= q;
//	}

//	/* mix full and reduced desired attitude */
//	Quatf q_mix = qd_red.inversed() * qd;
//	q_mix *= math::signNoZero(q_mix(0));
//	/* catch numerical problems with the domain of acosf and asinf */
//	q_mix(0) = math::constrain(q_mix(0), -1.f, 1.f);
//	q_mix(3) = math::constrain(q_mix(3), -1.f, 1.f);
//	qd = qd_red * Quatf(cosf(yaw_w * acosf(q_mix(0))), 0, 0, sinf(yaw_w * asinf(q_mix(3))));

//	/* quaternion attitude control law, qe is rotation from q to qd */
//	Quatf qe = q.inversed() * qd;

//	/* using sin(alpha/2) scaled rotation axis as attitude error (see quaternion definition by axis angle)
//	 * also taking care of the antipodal unit quaternion ambiguity */
//	Vector3f eq = 2.f * math::signNoZero(qe(0)) * qe.imag();

//	/* calculate angular rates setpoint */
//	_rates_sp = eq.emult(attitude_gain);

	/* Feed forward the yaw setpoint rate.
	 * yaw_sp_move_rate is the feed forward commanded rotation around the world z-axis,
	 * but we need to apply it in the body frame (because _rates_sp is expressed in the body frame).
	 * Therefore we infer the world z-axis (expressed in the body frame) by taking the last column of R.transposed (== q.inversed)
	 * and multiply it by the yaw setpoint rate (yaw_sp_move_rate).
	 * This yields a vector representing the commanded rotatation around the world z-axis expressed in the body frame
	 * such that it can be added to the rates setpoint.
	 */
//	_rates_sp += q.inversed().dcm_z() * _v_att_sp.yaw_sp_move_rate;


//	/* limit rates */
//	for (int i = 0; i < 3; i++) {
//		if ((_v_control_mode.flag_control_velocity_enabled || _v_control_mode.flag_control_auto_enabled) &&
//		    !_v_control_mode.flag_control_manual_enabled) {
//			_rates_sp(i) = math::constrain(_rates_sp(i), -_auto_rate_max(i), _auto_rate_max(i));

//		} else {
//			_rates_sp(i) = math::constrain(_rates_sp(i), -_mc_rate_max(i), _mc_rate_max(i));
//		}
//	}
}

/*
 * Throttle PID attenuation
 * Function visualization available here https://www.desmos.com/calculator/gn4mfoddje
 * Input: 'tpa_breakpoint', 'tpa_rate', '_thrust_sp'
 * Output: 'pidAttenuationPerAxis' vector
 */
Vector3f
MulticopterAttitudeControl::pid_attenuations(float tpa_breakpoint, float tpa_rate)
{
	/* throttle pid attenuation factor */
	float tpa = 1.0f - tpa_rate * (fabsf(_thrust_sp) - tpa_breakpoint) / (1.0f - tpa_breakpoint);
	tpa = fmaxf(TPA_RATE_LOWER_LIMIT, fminf(1.0f, tpa));

	Vector3f pidAttenuationPerAxis;
	pidAttenuationPerAxis(AXIS_INDEX_ROLL) = tpa;
	pidAttenuationPerAxis(AXIS_INDEX_PITCH) = tpa;
	pidAttenuationPerAxis(AXIS_INDEX_YAW) = 1.0;

	return pidAttenuationPerAxis;
}

/*
 * Attitude rates controller.
 * Input: '_rates_sp' vector, '_thrust_sp'
 * Output: '_att_control' vector
 */
void
MulticopterAttitudeControl::control_attitude_rates(float dt)
{
	/* reset integral if disarmed */
	if (!_v_control_mode.flag_armed || !_vehicle_status.is_rotary_wing) {
		_rates_int.zero();
	}

	// get the raw gyro data and correct for thermal errors
	Vector3f rates;

	if (_selected_gyro == 0) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_0[0]) * _sensor_correction.gyro_scale_0[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_0[1]) * _sensor_correction.gyro_scale_0[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_0[2]) * _sensor_correction.gyro_scale_0[2];

	} else if (_selected_gyro == 1) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_1[0]) * _sensor_correction.gyro_scale_1[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_1[1]) * _sensor_correction.gyro_scale_1[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_1[2]) * _sensor_correction.gyro_scale_1[2];

	} else if (_selected_gyro == 2) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_2[0]) * _sensor_correction.gyro_scale_2[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_2[1]) * _sensor_correction.gyro_scale_2[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_2[2]) * _sensor_correction.gyro_scale_2[2];

	} else {
		rates(0) = _sensor_gyro.x;
		rates(1) = _sensor_gyro.y;
		rates(2) = _sensor_gyro.z;
	}

	// rotate corrected measurements from sensor to body frame
	rates = _board_rotation * rates;

	// correct for in-run bias errors
	rates(0) -= _sensor_bias.gyro_x_bias;
	rates(1) -= _sensor_bias.gyro_y_bias;
	rates(2) -= _sensor_bias.gyro_z_bias;

//	Vector3f rates_p_scaled = _rate_p.emult(pid_attenuations(_tpa_breakpoint_p.get(), _tpa_rate_p.get()));
//	Vector3f rates_i_scaled = _rate_i.emult(pid_attenuations(_tpa_breakpoint_i.get(), _tpa_rate_i.get()));
//	Vector3f rates_d_scaled = _rate_d.emult(pid_attenuations(_tpa_breakpoint_d.get(), _tpa_rate_d.get()));

//	/* angular rates error */
//	Vector3f rates_err = _rates_sp - rates;

//	/* apply low-pass filtering to the rates for D-term */
//	Vector3f rates_filtered(_lp_filters_d.apply(rates));

//	_att_control = rates_p_scaled.emult(rates_err) +
//		       _rates_int -
//		       rates_d_scaled.emult(rates_filtered - _rates_prev_filtered) / dt +
//		       _rate_ff.emult(_rates_sp);

//	_rates_prev = rates;
//	_rates_prev_filtered = rates_filtered;

//	/* update integral only if we are not landed */
//	if (!_vehicle_land_detected.maybe_landed && !_vehicle_land_detected.landed) {
//		for (int i = AXIS_INDEX_ROLL; i < AXIS_COUNT; i++) {
//			// Check for positive control saturation
//			bool positive_saturation =
//				((i == AXIS_INDEX_ROLL) && _saturation_status.flags.roll_pos) ||
//				((i == AXIS_INDEX_PITCH) && _saturation_status.flags.pitch_pos) ||
//				((i == AXIS_INDEX_YAW) && _saturation_status.flags.yaw_pos);

//			// Check for negative control saturation
//			bool negative_saturation =
//				((i == AXIS_INDEX_ROLL) && _saturation_status.flags.roll_neg) ||
//				((i == AXIS_INDEX_PITCH) && _saturation_status.flags.pitch_neg) ||
//				((i == AXIS_INDEX_YAW) && _saturation_status.flags.yaw_neg);

//			// prevent further positive control saturation
//			if (positive_saturation) {
//				rates_err(i) = math::min(rates_err(i), 0.0f);

//			}

//			// prevent further negative control saturation
//			if (negative_saturation) {
//				rates_err(i) = math::max(rates_err(i), 0.0f);

//			}

//			// Perform the integration using a first order method and do not propagate the result if out of range or invalid
//			float rate_i = _rates_int(i) + rates_i_scaled(i) * rates_err(i) * dt;

//			if (PX4_ISFINITE(rate_i) && rate_i > -_rate_int_lim(i) && rate_i < _rate_int_lim(i)) {
//				_rates_int(i) = rate_i;

//			}
//		}
//	}

//	/* explicitly limit the integrator state */
//	for (int i = AXIS_INDEX_ROLL; i < AXIS_COUNT; i++) {
//		_rates_int(i) = math::constrain(_rates_int(i), -_rate_int_lim(i), _rate_int_lim(i));

//	}




    /*----CYW_ PX4 Modern Control--START------*/
    /*Author:Yiwen Chen*/
    /*Time:2019.01.16*/
    /*git version:exp20190116v190aCYWMCv110*/
    /*Abstract: Using Modern Control System to Control PX4 keep height*/
    /*Update Log:
     * v100:
     *       Using Modern Control System to Control PX4 keep height
     * v110:
     *      1.Simple Flight Mission:1m 1.5m 2m
     *      2.Safely return back to 0m
    */

if(cywmc_able){

    /*Init running Time*/
    if(_v_control_mode.flag_armed && _v_control_mode.flag_control_rates_enabled ){
        if(arm_t0<1){
        arm_t0=hrt_absolute_time();
        }
        run_t=(hrt_absolute_time()-arm_t0)/1000000;


    }
    else{
        arm_t0=0;
        run_t=0;
    }


    //some cache (wasted)
    if(0){
        //    bool updated;
        //    vehicle_status_s status_current;
        //    _vehicle_status_sub = orb_subscribe(ORB_ID(vehicle_status));

        //    orb_check(_vehicle_status_sub, &updated);
        //    if (updated) {
        //        orb_copy(ORB_ID(vehicle_status), _vehicle_status_sub, &status_current);
        //    }


        //    if(_v_control_mode.flag_armed ){
        //        if(arm_t0<1){
        //        arm_t0=hrt_absolute_time();
        //        }

        //    }

        //    if(_v_control_mode.flag_armed){
        //        run_t=(hrt_absolute_time()-arm_t0)/1000000;
        //    }
        //    else{
        //        run_t=0;
        //    }
    }

    /*Print Status For Checking Error*/
    loop_times+=1;
    if(loop_times%show_per_times==1){
            //    PX4_INFO("arm_t0=%f",(double)arm_t0);
            //    PX4_INFO("run_t=%f",(double)run_t);
            //    PX4_INFO("flag_control_manual_enabled=%d",(bool)_v_control_mode.flag_control_manual_enabled);
            //    PX4_INFO("nav_state=%c",(bool)status_current.nav_state);

                //PX4_INFO("flag_control_manual_enabled=%d",(bool)_v_control_mode.flag_control_manual_enabled);
                }

    /*Get Current State */
      /*1.get current position data*/
        int pos_sub_fd = orb_subscribe(ORB_ID(vehicle_local_position));
        //struct vehicle_local_position_s pos_current;
        //orb_set_interval(pos_sub_fd, 200);
        bool updated;
        orb_check(pos_sub_fd, &updated);
        if (updated) {
            orb_copy(ORB_ID(vehicle_local_position), pos_sub_fd, &pos_current);
        }
            //some cache (wasted) _sub fd
            if(0){
            //        px4_pollfd_struct_t fds[] = {
            //            { .fd = pos_sub_fd,   .events = POLLIN },
            //        };
            //        int poll_ret = px4_poll(pos_sub_fd, 1,200);
            //        if (poll_ret > 0) {
            //            if (fds[0].revents & POLLIN) {
            //                    orb_copy(ORB_ID(vehicle_local_position), pos_sub_fd, &pos_current);
            //                    //orb_publish(ORB_ID(vehicle_attitude), att_pub, &att);
            //                }
            //        }
            }
      /*2.get current attitude data*/
        Quatf q_current(_v_att.q);

        float Z,agl_roll,agl_pitch,agl_yaw,rate_Z,rate_roll,rate_pitch,rate_yaw;
        Z=pos_current.z;
        agl_roll=Eulerf(q_current).phi();;
        agl_pitch=Eulerf(q_current).theta();
        agl_yaw=Eulerf(q_current).psi();
        rate_Z=pos_current.vz;
        rate_roll=rates(0);
        rate_pitch=rates(1);
        rate_yaw=rates(2);
            //            PX4_INFO("current_state=\n%f\n%f\n%f\n%f\n%f\n%f\n%f\n%f",
            //                     (double)Z,(double)agl_roll,
            //                     (double)agl_pitch,(double)agl_yaw,
            //                     (double)rate_Z,(double)rate_roll,
            //                     (double)rate_pitch,(double)rate_yaw);





    /*Init for default parameters in SS.*/
    if(ss_seted==0){
        /*All set zeros*/
        ss_x.setZero();
        ss_x_dot.setZero();
        ss_r.setZero();
        ss_u_scale.setZero();
        ss_u_actual.setZero();
        ss_y.setZero();
        ss_k.setZero();
        ss_A.setZero();
        ss_B.setZero();
        ss_C.setZero();
        ss_D.setZero();
        //ss_G.setZero();
        //ss_G1.setZero();
        //ss_I4.setIdentity();
        ss_G_scale.setZero();



        ss_G_scale(0,0)=ss_G_scale_0;
        ss_G_scale(1,1)=0;
        ss_G_scale(2,2)=0;
        ss_G_scale(3,3)=0;

        /*r*/
        ss_r(0,0)=setout_Z;
        ss_r(0,1)=0;
        ss_r(0,2)=0;
        ss_r(0,3)=0;
        ss_r=ss_G_scale*ss_r;

        /*A*/
        ss_A(0,4)=1;
        ss_A(1,5)=1;
        ss_A(2,6)=1;
        ss_A(3,7)=1;

        /*B*/
        ss_B(4,0)=1/ss_m;
        ss_B(5,1)=1/ss_Ixx;
        ss_B(6,2)=1/ss_Iyy;
        ss_B(7,3)=1/ss_Izz;

        /*C*/
        ss_C(0,0)=1;
        ss_C(1,1)=1;
        ss_C(2,2)=1;
        ss_C(3,3)=1;

        /*D*/
        //=0


        /*K*/
        ss_k(0,0)=3;
        ss_k(0,4)=4;
        ss_k(1,1)=1.4800;
        ss_k(1,2)=-0.3884;
        ss_k(1,3)=-0.3751;
        ss_k(1,5)=0.5151;
        ss_k(1,6)=-0.0914;
        ss_k(1,7)=-0.0650;
        ss_k(2,1)=0.1841;
        ss_k(2,2)=1.6960;
        ss_k(2,3)=-0.0133;
        ss_k(2,5)=0.0709;
        ss_k(2,6)=0.5454;
        ss_k(2,7)=0.0140;
        ss_k(3,1)=0.9353;
        ss_k(3,2)=0.6032;
        ss_k(3,3)=5.4446;
        ss_k(3,5)=0.0915;
        ss_k(3,6)=0.1529;
        ss_k(3,7)=2.1972;

        /*Setout y*/
        ss_setout_y(0,0)=setout_Z;
        ss_setout_y(0,1)=setout_phi;
        ss_setout_y(0,2)=setout_theta;
        ss_setout_y(0,3)=setout_psi;


        /*G*/

        //ss_G1=ss_C*1;
        //ss_G.setZero();

        /*SS Finished Initiatsing*/
        ss_seted=1;

        //ss_A=ss_A*ss_A;
        //PX4_INFO("ss_C=%f",(double)ss_A(1,1));
        //ss_A=ss_A.inversed();
    }

    /*Update r if needed in every loop*/
    if(need_update_r){
        /*r*/
        ss_r(0,0)=setout_Z;
        ss_r=ss_G_scale*ss_r;
        need_update_r=0;
    }
    /*Mission*/
    if(modern_control_mission_able){
        mordern_control_able=1;
        give_output_able=1;
        //Mission 1 //1m 1.5m 2m
        if(modern_control_mission_select==1){
            //Init Time Plan
            if(!is_msl_t_set){

            ms1_t[0]=5;//keep still
            ms1_t[1]=15;//rise and keep at 1m
            ms1_t[2]=15;//rise and keep at 1.5m
            ms1_t[3]=15;//rise and keep at 2m

            is_msl_t_set=1;
            }

            //Mission Content
            if(run_t<ms1_t[0]){
                give_output_able=0;
                _att_control(0)=0;//roll
                _att_control(1)=0;//pitch
                _att_control(2)=0;//yaw
                _thrust_sp=0;
            }
            else if(run_t>ms1_t[0] && run_t<(ms1_t[0]+ms1_t[1])){
                need_update_r=1;
                setout_Z=1;
            }
            else if(run_t>ms1_t[0] && run_t<(ms1_t[0]+ms1_t[1]+ms1_t[2])){
                need_update_r=1;
                setout_Z=1.5;
            }
            else if(run_t>ms1_t[0] && run_t<(ms1_t[0]+ms1_t[1]+ms1_t[2]+ms1_t[3])){
                need_update_r=1;
                setout_Z=2;
            }
            else{
                return_back_to_0m_able=1;
                modern_control_mission_select=0;
                start_return_back_runt=run_t;
            }
            if(loop_times%show_per_times==1){
                PX4_INFO("runt=%f",(double)run_t);
            }

        }

        //Mission 2 //x m
        if(modern_control_mission_select==2){
            //Init Time Plan
            if(!is_msl_t_set){

            ms1_t[0]=mission_2_time_still;//keep still
            ms1_t[1]=mission_2_time_height;//rise and keep at 1m

            is_msl_t_set=1;
            }

            //Mission Content
            if(run_t<ms1_t[0]){
                give_output_able=0;
                _att_control(0)=0;//roll
                _att_control(1)=0;//pitch
                _att_control(2)=0;//yaw
                _thrust_sp=0;
            }
            else if(run_t>ms1_t[0] && run_t<(ms1_t[0]+ms1_t[1])){
                need_update_r=1;
                setout_Z=mission_2_setout_Z;
            }
            else{
                return_back_to_0m_able=1;
                modern_control_mission_select=0;
                start_return_back_runt=run_t;
            }
            if(loop_times%show_per_times==1){
                PX4_INFO("runt=%f",(double)run_t);
            }
        }
        //Mission 3 //5m
        if(modern_control_mission_select==3){
//            //Init Time Plan
//            if(!is_msl_t_set){

//            ms1_t[0]=3;//keep still
//            ms1_t[1]=50;//rise and keep at 1m

//            is_msl_t_set=1;
//            }

//            //Mission Content
//            if(run_t<ms1_t[0]){
//                give_output_able=0;
//                _att_control(0)=0;//roll
//                _att_control(1)=0;//pitch
//                _att_control(2)=0;//yaw
//                _thrust_sp=0;
//            }
//            else if(run_t>ms1_t[0] && run_t<(ms1_t[0]+ms1_t[1])){
//                need_update_r=1;
//                setout_Z=5;
//            }
//            else{
//                return_back_to_0m_able=1;
//                modern_control_mission_select=0;
//                start_return_back_runt=run_t;
//            }
//            if(loop_times%show_per_times==1){
//                PX4_INFO("runt=%f",(double)run_t);
//            }
        }

    }
    else{
        //stand still
        give_output_able=0;
//        _att_control(0)=0;//roll
//        _att_control(1)=0;//pitch
//        _att_control(2)=0;//yaw
//        _thrust_sp=0;
    }

    /*Return Back To 0m*/
    if(run_t>safety_return_time){
        cywmc_able=0;
    }
    if(return_back_to_0m_able){



        if(loop_times%show_per_times==1){
            //PX4_INFO("I'm Now Going back to %f",(double)setout_Z);
        }
        need_update_r=1;

        if(-Z<back_Z_aim/return_degress_rate && setout_Z>=0.25f){
            back_Z_aim=-Z*return_degress_rate;
            setout_Z=setout_Z*return_degress_rate;
            PX4_INFO("RETURN Z UPDATED!:I'm Now Going back to %f",(double)setout_Z);
        }


        //end return
        if(setout_Z<0.25f){
            setout_Z=0;
            need_update_r=1;
        }
        if(-Z<0.2f){

            //stand still
            give_output_able=0;
//            _att_control(0)=0;//roll
//            _att_control(1)=0;//pitch
//            _att_control(2)=0;//yaw
//            _thrust_sp=0;
//            setout_Z=0;
            //return_back_to_0m_able=0;
//            need_update_r=1;
        }

    }

    if(loop_times%show_per_times==1){
        PX4_INFO("I'm Now Going to h=%f m",(double)setout_Z);
    }

    /* Main feedback control */
    if(mordern_control_able){
        ss_x(0,0)=-Z;
        ss_x(0,1)=agl_roll;
        ss_x(0,2)=agl_pitch;
        ss_x(0,3)=agl_yaw;
        ss_x(0,4)=-rate_Z;
        ss_x(0,5)=rate_roll;
        ss_x(0,6)=rate_pitch;
        ss_x(0,7)=rate_yaw;
        ss_u_actual=ss_r-ss_k*ss_x;
        ss_x_dot=ss_B*ss_u_actual+ss_A*ss_x;
        ss_x=ss_x+ss_x_dot*dt;
        ss_y=ss_C*ss_x;
        //PX4_INFO("dt=\n%f",(double)dt);
    }

    /*Give Output*/
    if(give_output_able){
        //update max_M and max_T
        if(1){
//            if(get_acc_able){
//                this_z=-Z;
//                this_vel=-pos_current.vz;
//                if(last_z>0.f && last_vel>0.f){this_acc=(this_vel-last_vel)/dt;}
//                last_z=this_z;
//                last_vel=this_vel;
//                //PX4_INFO("this_acc=%f",(double)this_acc);
//                //PX4_INFO("this_vel=%f",(double)this_vel);
//            }
//            //update T_max
//            if(this_acc>0.f && this_acc < 40.f){
//            T_now=ss_m*(this_acc+g);
//            if(T_now>T_max){
//                T_max=T_now;
//                PX4_INFO("update this_acc=%f",(double)this_acc);
//            }
//            }
//            if(abs(ss_u_actual(0,1))>Mx_max){
//                Mx_max=abs(ss_u_actual(0,1));
//            }
//            if(abs(ss_u_actual(0,2))>My_max){
//                My_max=abs(ss_u_actual(0,2));
//            }
//            if(abs(ss_u_actual(0,3))>Mz_max){
//                Mz_max=abs(ss_u_actual(0,3));
//            }

        }
        //actual 为完成稳定，需要的力矩和力，
        //T_max为(最大加速度+g)*m
        //M_max为(最大角加速度)*J
        //        ss_u_scale(0,0)=(ss_u_actual(0,0)+ss_m*g)/T_max;

        //R可以为0,但u一直不为0。
        ss_u_scale(0,0)=(ss_u_actual(0,0)+ss_m*g)/T_max;
        //ss_u_scale(0,0)=ss_u_actual(0,0)/T_max;
        ss_u_scale(0,1)=ss_u_actual(0,1)/Mx_max;
        ss_u_scale(0,2)=ss_u_actual(0,2)/My_max;
        ss_u_scale(0,3)=ss_u_actual(0,3)/Mz_max;
        if(cywmc_angle_control){
        _att_control(0)=ss_u_scale(0,1);//roll
        _att_control(1)=ss_u_scale(0,2);//pitch
        _att_control(2)=ss_u_scale(0,3);//yaw
        }
        _thrust_sp=ss_u_scale(0,0);//trust
        //_att_control(3)=ss_u_scale(0,0);//thrust //Make it's bigger than gravity
        }
    else{
        if(cywmc_angle_control){
        _att_control(0)=0;//roll
        _att_control(1)=0;//pitch
        _att_control(2)=0;//yaw
        }
        _thrust_sp=0;
    }

    /*Print Status Data to Chekc Error*/
    if(loop_times%show_per_times==1){
    PX4_INFO("_att_control=\n%f\n%f\n%f\n%f",
             (double)_thrust_sp,
             (double)_att_control(0),
             (double)_att_control(1),
             (double)_att_control(2)
             );
        PX4_INFO("ss_u_actual=\n%f\n%f\n%f\n%f",
                 (double)ss_u_actual(0,0),
                 (double)ss_u_actual(0,1),
                 (double)ss_u_actual(0,2),
                 (double)ss_u_actual(0,3));
        PX4_INFO("MAX_VALUE=\n%f\n%f\n%f\n%f",
                 (double)T_max,
                 (double)Mx_max,
                 (double)My_max,
                 (double)Mz_max
                 );
//    PX4_INFO("ss_set=\n%d",
//             (int)ss_seted);
//    PX4_INFO("ss_r=\n%f\n%f\n%f\n%f",
//             (double)ss_r(0,0),
//             (double)ss_r(0,1),
//             (double)ss_r(0,2),
//             (double)ss_r(0,3));
//    PX4_INFO("ss_k*ss_x=\n%f\n%f\n%f\n%f",
//             (double)ss_k*ss_x(0,0),
//             (double)ss_k*ss_x(0,1),
//             (double)ss_k*ss_x(0,2),
//             (double)ss_k*ss_x(0,3));

    PX4_INFO("ss_y=\n%f\n%f\n%f\n%f",
             (double)ss_y(0,0),
             (double)ss_y(0,1),
             (double)ss_y(0,2),
             (double)ss_y(0,3));
}


    /*End of CYW_MC*/
    if(1){
        orb_unsubscribe(pos_sub_fd);
    //    orb_unsubscribe(_vehicle_status_sub);
}

}

}

//void scale_output(){
//    _att_control(0)=0;//roll
//    _att_control(1)=0;//pitch
//    _att_control(2)=0;//yaw
//    _thrust_sp=0;
//}
/*----CYW_ PX4 Modern Control--END------*/

void
MulticopterAttitudeControl::publish_rates_setpoint()
{
//	_v_rates_sp.roll = _rates_sp(0);
//	_v_rates_sp.pitch = _rates_sp(1);
//	_v_rates_sp.yaw = _rates_sp(2);
//	_v_rates_sp.thrust_body[0] = 0.0f;
//	_v_rates_sp.thrust_body[1] = 0.0f;
//	_v_rates_sp.thrust_body[2] = -_thrust_sp;
//	_v_rates_sp.timestamp = hrt_absolute_time();
//	orb_publish_auto(ORB_ID(vehicle_rates_setpoint), &_v_rates_sp_pub, &_v_rates_sp, nullptr, ORB_PRIO_DEFAULT);
}

void
MulticopterAttitudeControl::publish_rate_controller_status()
{
	rate_ctrl_status_s rate_ctrl_status;
	rate_ctrl_status.timestamp = hrt_absolute_time();
	rate_ctrl_status.rollspeed = _rates_prev(0);
	rate_ctrl_status.pitchspeed = _rates_prev(1);
	rate_ctrl_status.yawspeed = _rates_prev(2);
	rate_ctrl_status.rollspeed_integ = _rates_int(0);
	rate_ctrl_status.pitchspeed_integ = _rates_int(1);
	rate_ctrl_status.yawspeed_integ = _rates_int(2);
	orb_publish_auto(ORB_ID(rate_ctrl_status), &_controller_status_pub, &rate_ctrl_status, nullptr, ORB_PRIO_DEFAULT);
}

void
MulticopterAttitudeControl::publish_actuator_controls()
{
	_actuators.control[0] = (PX4_ISFINITE(_att_control(0))) ? _att_control(0) : 0.0f;
	_actuators.control[1] = (PX4_ISFINITE(_att_control(1))) ? _att_control(1) : 0.0f;
	_actuators.control[2] = (PX4_ISFINITE(_att_control(2))) ? _att_control(2) : 0.0f;
	_actuators.control[3] = (PX4_ISFINITE(_thrust_sp)) ? _thrust_sp : 0.0f;
	_actuators.control[7] = (float)_landing_gear.landing_gear;
	_actuators.timestamp = hrt_absolute_time();
	_actuators.timestamp_sample = _sensor_gyro.timestamp;

	/* scale effort by battery status */
	if (_bat_scale_en.get() && _battery_status.scale > 0.0f) {
		for (int i = 0; i < 4; i++) {
			_actuators.control[i] *= _battery_status.scale;
		}
	}

	if (!_actuators_0_circuit_breaker_enabled) {
		orb_publish_auto(_actuators_id, &_actuators_0_pub, &_actuators, nullptr, ORB_PRIO_DEFAULT);
	}
}

void
MulticopterAttitudeControl::run()
{

	/*
	 * do subscriptions
	 */
	_v_att_sub = orb_subscribe(ORB_ID(vehicle_attitude));
	_v_att_sp_sub = orb_subscribe(ORB_ID(vehicle_attitude_setpoint));
	_v_rates_sp_sub = orb_subscribe(ORB_ID(vehicle_rates_setpoint));
	_v_control_mode_sub = orb_subscribe(ORB_ID(vehicle_control_mode));
	_params_sub = orb_subscribe(ORB_ID(parameter_update));
	_manual_control_sp_sub = orb_subscribe(ORB_ID(manual_control_setpoint));
	_vehicle_status_sub = orb_subscribe(ORB_ID(vehicle_status));
	_motor_limits_sub = orb_subscribe(ORB_ID(multirotor_motor_limits));
	_battery_status_sub = orb_subscribe(ORB_ID(battery_status));

	_gyro_count = math::min(orb_group_count(ORB_ID(sensor_gyro)), MAX_GYRO_COUNT);

	if (_gyro_count == 0) {
		_gyro_count = 1;
	}

	for (unsigned s = 0; s < _gyro_count; s++) {
		_sensor_gyro_sub[s] = orb_subscribe_multi(ORB_ID(sensor_gyro), s);
	}

	_sensor_correction_sub = orb_subscribe(ORB_ID(sensor_correction));
	_sensor_bias_sub = orb_subscribe(ORB_ID(sensor_bias));
	_vehicle_land_detected_sub = orb_subscribe(ORB_ID(vehicle_land_detected));
	_landing_gear_sub = orb_subscribe(ORB_ID(landing_gear));

	/* wakeup source: gyro data from sensor selected by the sensor app */
	px4_pollfd_struct_t poll_fds = {};
	poll_fds.events = POLLIN;

	const hrt_abstime task_start = hrt_absolute_time();
	hrt_abstime last_run = task_start;
	float dt_accumulator = 0.f;
	int loop_counter = 0;

	bool reset_yaw_sp = true;
	float attitude_dt = 0.f;

	while (!should_exit()) {

		poll_fds.fd = _sensor_gyro_sub[_selected_gyro];

		/* wait for up to 100ms for data */
		int pret = px4_poll(&poll_fds, 1, 100);

		/* timed out - periodic check for should_exit() */
		if (pret == 0) {
			continue;
		}

		/* this is undesirable but not much we can do - might want to flag unhappy status */
		if (pret < 0) {
			PX4_ERR("poll error %d, %d", pret, errno);
			/* sleep a bit before next try */
			usleep(100000);
			continue;
		}

		perf_begin(_loop_perf);

		/* run controller on gyro changes */
		if (poll_fds.revents & POLLIN) {
			const hrt_abstime now = hrt_absolute_time();
			float dt = (now - last_run) / 1e6f;
			last_run = now;

			/* guard against too small (< 0.2ms) and too large (> 20ms) dt's */
			if (dt < 0.0002f) {
				dt = 0.0002f;

			} else if (dt > 0.02f) {
				dt = 0.02f;
			}

			/* copy gyro data */
			orb_copy(ORB_ID(sensor_gyro), _sensor_gyro_sub[_selected_gyro], &_sensor_gyro);

			/* run the rate controller immediately after a gyro update */
			if (_v_control_mode.flag_control_rates_enabled) {
				control_attitude_rates(dt);

				publish_actuator_controls();
				publish_rate_controller_status();
			}

			/* check for updates in other topics */
			vehicle_control_mode_poll();
			vehicle_status_poll();
			vehicle_motor_limits_poll();
			battery_status_poll();
			sensor_correction_poll();
			sensor_bias_poll();
			vehicle_land_detected_poll();
			landing_gear_state_poll();
			const bool manual_control_updated = vehicle_manual_poll();
			const bool attitude_updated = vehicle_attitude_poll();
			attitude_dt += dt;

			/* Check if we are in rattitude mode and the pilot is above the threshold on pitch
			 * or roll (yaw can rotate 360 in normal att control). If both are true don't
			 * even bother running the attitude controllers */
			if (_v_control_mode.flag_control_rattitude_enabled) {
				_v_control_mode.flag_control_attitude_enabled =
						fabsf(_manual_control_sp.y) <= _rattitude_thres.get() &&
						fabsf(_manual_control_sp.x) <= _rattitude_thres.get();
			}

			bool attitude_setpoint_generated = false;

			if (_v_control_mode.flag_control_attitude_enabled && _vehicle_status.is_rotary_wing) {
				if (attitude_updated) {
					// Generate the attitude setpoint from stick inputs if we are in Manual/Stabilized mode
					if (_v_control_mode.flag_control_manual_enabled &&
							!_v_control_mode.flag_control_altitude_enabled &&
							!_v_control_mode.flag_control_velocity_enabled &&
							!_v_control_mode.flag_control_position_enabled) {
						generate_attitude_setpoint(attitude_dt, reset_yaw_sp);
						attitude_setpoint_generated = true;
					}

					control_attitude();
					publish_rates_setpoint();
				}

			} else {
				/* attitude controller disabled, poll rates setpoint topic */
				if (_v_control_mode.flag_control_manual_enabled && _vehicle_status.is_rotary_wing) {
					if (manual_control_updated) {
						/* manual rates control - ACRO mode */
						Vector3f man_rate_sp(
								math::superexpo(_manual_control_sp.y, _acro_expo_rp.get(), _acro_superexpo_rp.get()),
								math::superexpo(-_manual_control_sp.x, _acro_expo_rp.get(), _acro_superexpo_rp.get()),
								math::superexpo(_manual_control_sp.r, _acro_expo_y.get(), _acro_superexpo_y.get()));
						_rates_sp = man_rate_sp.emult(_acro_rate_max);
						_thrust_sp = _manual_control_sp.z;
						publish_rates_setpoint();
					}

				} else {
					/* attitude controller disabled, poll rates setpoint topic */
					if (vehicle_rates_setpoint_poll()) {
						_rates_sp(0) = _v_rates_sp.roll;
						_rates_sp(1) = _v_rates_sp.pitch;
						_rates_sp(2) = _v_rates_sp.yaw;
						_thrust_sp = -_v_rates_sp.thrust_body[2];
					}
				}
			}

			if (_v_control_mode.flag_control_termination_enabled) {
				if (!_vehicle_status.is_vtol) {
					_rates_sp.zero();
					_rates_int.zero();
					_thrust_sp = 0.0f;
					_att_control.zero();
					publish_actuator_controls();
				}
			}

			if (attitude_updated) {
				reset_yaw_sp = (!attitude_setpoint_generated && !_v_control_mode.flag_control_rattitude_enabled) ||
						_vehicle_land_detected.landed ||
						(_vehicle_status.is_vtol && !_vehicle_status.is_rotary_wing); // VTOL in FW mode
				attitude_dt = 0.f;
			}

			/* calculate loop update rate while disarmed or at least a few times (updating the filter is expensive) */
			if (!_v_control_mode.flag_armed || (now - task_start) < 3300000) {
				dt_accumulator += dt;
				++loop_counter;

				if (dt_accumulator > 1.f) {
					const float loop_update_rate = (float)loop_counter / dt_accumulator;
					_loop_update_rate_hz = _loop_update_rate_hz * 0.5f + loop_update_rate * 0.5f;
					dt_accumulator = 0;
					loop_counter = 0;
					_lp_filters_d.set_cutoff_frequency(_loop_update_rate_hz, _d_term_cutoff_freq.get());
				}
			}

			parameter_update_poll();
		}

		perf_end(_loop_perf);
	}

	orb_unsubscribe(_v_att_sub);
	orb_unsubscribe(_v_att_sp_sub);
	orb_unsubscribe(_v_rates_sp_sub);
	orb_unsubscribe(_v_control_mode_sub);
	orb_unsubscribe(_params_sub);
	orb_unsubscribe(_manual_control_sp_sub);
	orb_unsubscribe(_vehicle_status_sub);
	orb_unsubscribe(_motor_limits_sub);
	orb_unsubscribe(_battery_status_sub);

	for (unsigned s = 0; s < _gyro_count; s++) {
		orb_unsubscribe(_sensor_gyro_sub[s]);
	}

	orb_unsubscribe(_sensor_correction_sub);
	orb_unsubscribe(_sensor_bias_sub);
	orb_unsubscribe(_vehicle_land_detected_sub);
	orb_unsubscribe(_landing_gear_sub);
}

int MulticopterAttitudeControl::task_spawn(int argc, char *argv[])
{
	_task_id = px4_task_spawn_cmd("mc_att_control",
					   SCHED_DEFAULT,
					   SCHED_PRIORITY_ATTITUDE_CONTROL,
					   1700,
					   (px4_main_t)&run_trampoline,
					   (char *const *)argv);

	if (_task_id < 0) {
		_task_id = -1;
		return -errno;
	}

	return 0;
}

MulticopterAttitudeControl *MulticopterAttitudeControl::instantiate(int argc, char *argv[])
{
	return new MulticopterAttitudeControl();
}

int MulticopterAttitudeControl::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int mc_att_control_main(int argc, char *argv[])
{
	return MulticopterAttitudeControl::main(argc, argv);
}
