/*!	\file position_mode.c
	\brief Functions for Profile Positon Mode
*/

#include "amc.h"
#include "debug_sci.h"
#include "position_mode.h"
#include "pid_reg.h"
#include "vectorial_current_mode.h"
#include "dc_bldc_current.h"

int pp_following_target = 0;	/*!< Indicates wether we are going to a target or just in initial stop trajectory */
int position_error_mms =0;
unsigned int debug_trajectory =0;
long derivada_pos[2]={0};
long vel_ref;
short int vel_ref_filt;

static long max_travel=0;

#define FREQ_POSITION_LOOP 10 // 10*1ms
#define FREQ_VELOCITY_LOOP 10 // 10*1ms


long pp_get_max_travel(void)
{
	return max_travel;
}

void pp_set_max_travel( long travel )
{
//	if( !max_travel )
		max_travel = travel;
}

void pp_set_pid_tunning( void )
{
	unsigned long ul_tmp;

	ul_tmp = Position_control_parameter_set[0];		/* Kp (has to be divided by Divisor) */
	ul_tmp = ul_tmp << 16;							/* multiply by 2^16 */
	ul_tmp /= Position_control_parameter_set[4];			/* divide by Divisor */
	// position_control_pid.Kp = _IQmpyI32(_IQ(0.0000152587890625),ul_tmp);	/* divide by 2^16 */
	position_control_pid.Kp = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */
	position_control_pid_mms.Kp = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */

	ul_tmp = Position_control_parameter_set[1];		/* Ki (has to be divided by Divisor) */
	ul_tmp = ul_tmp << 16;						/* multiply by 2^16 */
	ul_tmp /= Position_control_parameter_set[4];			/* divide by Divisor */

	position_control_pid.Ki = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */
	position_control_pid_mms.Ki = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */


	ul_tmp = Position_control_parameter_set[2];		/* Kd (has to be divided by Divisor) */
	ul_tmp = ul_tmp << 16;						/* multiply by 2^16 */
	ul_tmp /= Position_control_parameter_set[4];			/* divide by Divisor */

	position_control_pid.Kd = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */
	position_control_pid_mms.Kd = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */


	ul_tmp = Position_control_parameter_set[3];		/* Kc (has to be divided by Divisor) */
	ul_tmp = ul_tmp << 16;						/* multiply by 2^16 */
	ul_tmp /= Position_control_parameter_set[4];			/* divide by Divisor */

	position_control_pid.Kc = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */
	position_control_pid_mms.Kc = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */


	pos_error_normFactor = Position_control_margin;




	ul_tmp = Position_Velocity_control_parameter_set[0];
	ul_tmp = ul_tmp << 16;							/* multiply by 2^16 */
	ul_tmp /= Position_Velocity_control_parameter_set[4];			/* divide by Divisor */

	pos_vel_control_pid_mms.Kp = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */


	ul_tmp = Position_Velocity_control_parameter_set[1];
	ul_tmp = ul_tmp << 16;							/* multiply by 2^16 */
	ul_tmp /= Position_Velocity_control_parameter_set[4];			/* divide by Divisor */

	pos_vel_control_pid_mms.Ki 		= ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */


	ul_tmp = Position_Velocity_control_parameter_set[2];
	ul_tmp = ul_tmp << 16;							/* multiply by 2^16 */
	ul_tmp /= Position_Velocity_control_parameter_set[4];			/* divide by Divisor */

	pos_vel_control_pid_mms.Kd = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */


	ul_tmp = Position_Velocity_control_parameter_set[3];
	ul_tmp = ul_tmp << 16;							/* multiply by 2^16 */
	ul_tmp /= Position_Velocity_control_parameter_set[4];			/* divide by Divisor */

	pos_vel_control_pid_mms.Kc = ul_tmp << (GLOBAL_Q - 16);		/* convert to IQ and divide by 2^16 */



	set_Kp_current_control_dc_bldc(Position_Current_control_parameter_set[0],
								   Position_Current_control_parameter_set[4]);

	set_Ki_current_control_dc_bldc(Position_Current_control_parameter_set[1],
								   Position_Current_control_parameter_set[4]);

	set_Kd_current_control_dc_bldc(Position_Current_control_parameter_set[2],
								   Position_Current_control_parameter_set[4]);

	set_Kc_current_control_dc_bldc(Position_Current_control_parameter_set[3],
								   Position_Current_control_parameter_set[4]);


}


/*****************************************************************/
/*!	 Main function of Profile Position Mode
	\param state Cinematic state of the system
	\param init will be 1 if the mode has to be initialized (0 if not)
*/
/*****************************************************************/
int profile_position_mode_operation(motion_state_struct *state, char init)
{
	long Position_demand;
	long target;		/* for new target mailbox reception */
	long halt_pos;
	int reset_timers = 0;
	static long long pp_init_timer=0;
	unsigned long long tmp;
	int ret = 1;
	static unsigned short int ntimes_pos_loop=FREQ_POSITION_LOOP-1;
	static unsigned short int ntimes_vel_loop=FREQ_VELOCITY_LOOP-1;

	if(init)
	{
		pp_initialize(state);		/* initialize the mode if necessary */
		reset_timers = 1;
		_LOGmessage(0x0110, "pp_initialize", 0, 0);
		if( Device_status_word & BRAKE_MASKBIT )
			pp_init_timer = state->time;
	}
	else
	{
		tmp = (long long)(state->time - pp_init_timer) * 1000;
		tmp /= lcounts_p_s;
		if( tmp > pp_init_time )
		{
			ret = 0;
			/* modify trajectory if a new target arrived */
			if (MBX_pend(&pp_newtarget_mbox, &target, 0) && !(Device_control_word & HALT_MASKBIT))
			{
				_LOGmessage(0x0108, "New Target_position updated", 0, 0);
				pp_trajectory_generator(target, &pp_trajectory, state);
				pp_trajectory.next_target_active = 0;		/* clear any pending non-immediate target */
				Device_status_word |= SET_POINT_ACKNOWLEDGE_MASKBIT;
				Device_status_word &= ~TARGET_REACHED_MASKBIT;			/* Target reached bit = 0 */
				send_SWord = 1;
				pp_following_target = 1;
				reset_timers = 1;
			}
			/* modify next trajectory target if a non-immediate target arrived */
			if (MBX_pend(&pp_nexttarget_mbox, &target, 0))
			{
				_LOGmessage(0x0109, "Next Target_position updated", 0, 0);
				pp_trajectory.next_target = target;
				pp_trajectory.next_target_active = 1;
				/* pp_following_target = 1; */ /* will be set when this target becames active */
				/* reset_timers = 1; */	/* if we reset timers here, the new start is delayed */
			}
		}
	}
	/* get position demand according to current trajectory */
	Position_demand = pp_trajectory_point(&pp_trajectory, state->time);

	pp_set_pid_tunning();

	/* update Position_demand_value and Position_demand_value_in_increments */
	Position_demand_value_in_increments = Position_demand;			/* [inc] */
	Position_demand_value = int2ext_pos(Position_demand, Home_offset);		/* [position units] */

	/* update Following error */
	Following_error_actual_value = Position_actual_value - Position_demand_value;
	// _iq position_controller(long int demand, long int position)
	//  tmp = (long long)(position - demand) * Position_factor_Feed_constant;
	// control_effort = position_controller(Position_demand, state->position);

#if 1
	if (ntimes_pos_loop==FREQ_POSITION_LOOP-1)
	{
		control_effort_pos_mm =  position_controller_mm(Position_demand_value, Position_actual_value);
		ntimes_pos_loop = 0;
	}
	else
	{
		ntimes_pos_loop++;
	}
#endif
#if 0
	if (ntimes_vel_loop<FREQ_POSITION_LOOP/2)
	{
		control_effort_pos_mm = _IQdiv(_IQ(25.0),_IQ(100.0));
		ntimes_vel_loop++;
	}
	else
	{
		control_effort_pos_mm = _IQ(0.0);
		if (ntimes_vel_loop==FREQ_POSITION_LOOP-1)
			ntimes_vel_loop=0;
		else
			ntimes_vel_loop++;
	}
#endif

	control_effort_vel_mms =  pos_pv_velocity_controller_mms(control_effort_pos_mm, Velocity_actual_value);
#if 1
	control_effort = control_effort_vel_mms;
#endif
#if 0
	if (ntimes_vel_loop==FREQ_VELOCITY_LOOP-1)
	{
		control_effort = _IQ(0.2);
		ntimes_vel_loop = 0;
	}
	else
	{
		control_effort = _IQ(0.0);
		ntimes_vel_loop++;
	}
#endif
	/* Debug */
#ifdef DEBUG_POS_LOOP
	TickDebugSci14bytesPositionMode();
#endif

	/* check status flags using position units (not increments) */
	halt_pos = int2ext_pos(pp_trajectory.s4, Home_offset);
	pp_status_flags((long)Position_actual_value, (long)Position_demand_value, pp_trajectory.target, halt_pos, state, reset_timers);

	return ret;
}

_iq pos_pv_velocity_controller_mms(_iq demand, long int velocity)
{
	pos_vel_control_pid_mms.Ref = demand;
	pos_vel_control_pid_mms.Fdb = _IQdiv(velocity,Max_profile_velocity);//_IQsat(_IQdiv(velocity,Max_profile_velocity),_IQ(1.0),_IQ(-1.0));
	pos_vel_control_pid_mms.calc(&pos_vel_control_pid_mms);		/* calculate PID output */
	return pos_vel_control_pid_mms.Out;
}


/*****************************************************************/
/*!	 Calculates the PID output in Profile Position Mode
	\param demand Position demand for this control cycle
	\param position Current position
	\return PID output [-1,1]
*/
/*****************************************************************/
_iq position_controller_mm(long int demand, long int position)
{
	position_error_mms = (long long)(demand - position);
	position_control_pid_mms.Ref = _IQsat(_IQdiv(demand	,max_travel),_IQ(1.0),_IQ(-1.0));
	position_control_pid_mms.Fdb = _IQdiv(position	,max_travel);//_IQsat(_IQdiv(position	,max_travel),_IQ(1.0),_IQ(-1.0));
	position_control_pid_mms.calc(&position_control_pid_mms);		/* calculate PID output */
	return position_control_pid_mms.Out;
}

/*****************************************************************/
/*!	 Calculates the position demand for current control cycle
	\param trajectory Position trajectory being followed
	\param current_time Current time
	\return Position demand
*/
/*****************************************************************/
long pp_trajectory_point(position_trajectory_struct *trajectory, long long current_time)
{
	long long tmp;
	long ret;

	/* It depends on the motion profile type (by now only linear ramp mode used) */
	if(current_time >= trajectory->t4)
	{
		DINT;
		p_current_limit = &current_limit;	/* set current limit to use */
		EINT;
		debug_trajectory = 8192;
		/* S4 */
		ret = trajectory->s4;
	}
	else if(current_time >= trajectory->t3)
	{
		DINT;
		p_current_limit = &current_limit;	/* set current limit to use */
		EINT;
		debug_trajectory = 4096;

		/* S3 + v2(t-t3)+ 1/2 * a3 * (t - t3)^2 */
		tmp = (long long)((long)(current_time - trajectory->t3)) * trajectory->v2;
		tmp = trajectory->s3 + (tmp / lcounts_p_s);
		ret = tmp + half_acc_t2(trajectory->a3, (unsigned long)(current_time - trajectory->t3));

	}
	else if(current_time >= trajectory->t2)
	{
		DINT;
		p_current_limit = &current_limit;	/* set current limit to use */
		EINT;
		debug_trajectory = 2048;
		/* S2 + v2 * (t - t2) */
		tmp = (long long)((long)(current_time - trajectory->t2)) * trajectory->v2;
		ret = trajectory->s2 + (tmp / lcounts_p_s);

	}
	else if(current_time >= trajectory->t1)
	{
		/* set current limit to use (acceleration limit only if v1==0) */
		DINT;
		p_current_limit = (trajectory->v1 == 0)? &current_limit_acc : &current_limit;
		EINT;
		debug_trajectory = 1024;
		/* S1 + v1 * (t - t1) + 1/2 * a1 * (t - t1)^2 */
		tmp = (long long)((long)(current_time - trajectory->t1)) * trajectory->v1;
		tmp = trajectory->s1 + (tmp / lcounts_p_s);
		ret = tmp + half_acc_t2(trajectory->a1, (unsigned long)(current_time - trajectory->t1));

	}
	else
	{
		DINT;
		p_current_limit = &current_limit;	/* set current limit to use */
		EINT;

		/* S0 + v0(t-t0) + 1/2 * a0 * (t - t0)^2 */
		tmp = (long long)((long)(current_time - trajectory->t0)) * trajectory->v0;
		tmp = trajectory->s0 + (tmp / lcounts_p_s);
		ret = tmp + half_acc_t2(trajectory->a0, (unsigned long)(current_time - trajectory->t0));
	}

	ret = CLAMP( ext2int_pos(Software_position_limit_Min_position_limit, Home_offset),
		     ret,
		     ext2int_pos(Software_position_limit_Max_position_limit, Home_offset) );
	return ret;
}


/*****************************************************************/
/*!	 Calculates main points of the trajectory to follow

	It doesn't use the current state as initial point but the "demanded" state
	\param new_target New position target (in velocity units)
	\param trajectory Position trajectory to be created
	\param state Cinematic state of the system
*/
/*****************************************************************/
void pp_trajectory_generator(long new_target, position_trajectory_struct *trajectory, motion_state_struct *state)
{
	long limited_target;				/* in position units */
	long target_pos, vel, acc, dec;		/* in [inc] [inc/s] and [inc/s^2] */
	long delta_s;
	long long stop_length;
	motion_state_struct demand_state;
	long tmp;

	demand_state.time = state->time;
	demand_state.position = pp_trajectory_point(trajectory, demand_state.time);
	demand_state.velocity = pp_trajectory_vel(trajectory, demand_state.time);

	/* limit movements on one side if any limit has been reached */
	if (position_flags & MIN_POS_MASKBIT)
	{
		tmp = int2ext_pos(demand_state.position, Home_offset);
		if(new_target < tmp) new_target = tmp;
	}
	if (position_flags & MAX_POS_MASKBIT)
	{
		tmp = int2ext_pos(demand_state.position, Home_offset);
		if(new_target > tmp) new_target = tmp;
	}

	acc = Profile_acceleration;		/* will be limitated and converted into [inc/s^2] */
	dec = Profile_deceleration;		/* will be limitated and converted into [inc/s^2] */
	vel = Profile_velocity;			/* will be limitated and converted into [inc/s] */
	pp_limits(new_target, &limited_target, &target_pos, &vel, &acc, &dec);	/* calculate limited parameters in internal units */

	trajectory->target = limited_target;
	trajectory->t0 = demand_state.time;
	trajectory->s0 = demand_state.position;
	trajectory->s4 = target_pos;
	trajectory->a0 = (-1) * sign(demand_state.velocity) * dec;
	trajectory->v0 = demand_state.velocity;
	trajectory->v2 = vel;	/* the sign will be checked */
	trajectory->v1 = 0;		/* unless no stop point and vel>v2 */
	trajectory->v4 = 0;

	/* calculate the length it would take it to stop */
	stop_length = (long long)demand_state.velocity * demand_state.velocity;
	stop_length = -stop_length / (trajectory->a0 * 2);	/*  stop_length = -1/2 * (v0^2/a0)  (can be negative) */

	delta_s = (target_pos - demand_state.position);

	if((sign(delta_s) != sign(demand_state.velocity)) || (labs(delta_s) < llabs(stop_length)))
	{		/* there will be an intermediate stop */
		long long st;		/* stop time */

		/* set the stop time */
		st = (long long)demand_state.velocity * lcounts_p_s;
		st = -st / trajectory->a0;

		trajectory->t1 = trajectory->t0 + st;		/* st is in low resolution clock time units */

		/* set the stop position */
		trajectory->s1 = trajectory->s0 + stop_length;

		/* set the trajectory accelerations depending on the direction of movement */
		trajectory->a1 = sign(trajectory->s4 - trajectory->s1) * acc;
		trajectory->a3 = -sign(trajectory->s4 - trajectory->s1) * dec;

		/* calculate the rest of the trajectory as if beginning stopped */
		pp_trajectory_from_stop(trajectory);
	}
	else
	{		/* there will NOT be an intermediate stop */
		long long tmp;
		if(labs(demand_state.velocity) > vel)
		{	/* v1 will not be zero, deceleration before v2 */
			/* set the initial velocity of the first deceleration section */
			trajectory->v1 = demand_state.velocity;

			/* point 1 is now */
			trajectory->t1 = demand_state.time;
			trajectory->s1 = demand_state.position;

			/* set accelerations */
			trajectory->a1 = -sign(demand_state.velocity) * dec;
			trajectory->a3 = trajectory->a1;

			/* calculate the rest of the trajectory */
			pp_trajectory_2decs(trajectory);
		}
		else
		{		/* there will NOT be an intermediate stop (but I will consider a previous one) */
			/* set the trajectory accelerations depending on the direction of movement */
			trajectory->a1 = sign(delta_s) * acc;
			trajectory->a3 = -sign(delta_s) * dec;

			/* t1: set the stop time from a hypothetical previous stop */
			tmp = (long long)demand_state.velocity * lcounts_p_s;
			trajectory->t1 = trajectory->t0 - (tmp / trajectory->a1);

			/* s1: set the hypothetical previous stop position */
			tmp = (long long)demand_state.velocity * demand_state.velocity;
			trajectory->s1 = trajectory->s0 - (tmp / (trajectory->a1 * 2));	/*  s1 = s0 - 1/2 * v0^2/a1  */

			/* calculate the rest of the trajectory as if beginning stopped */
			pp_trajectory_from_stop(trajectory);
		}
	}
}


/*****************************************************************/
/*!	 Calculates a position trajectory starting from a stopped position (s1,t1)
	\param trajectory Position trajectory to be created
*/
/*****************************************************************/
void pp_trajectory_from_stop(position_trajectory_struct *trajectory)
{
	long long tmp;		/* 64-bit auxiliar variable */

	/* calculate the critical length for which Profile_velocity is not reached */
	tmp = (long long)(trajectory->a3 - trajectory->a1) * trajectory->v2;
	tmp /= trajectory->a1;
	tmp = (tmp * trajectory->v2) / trajectory->a3;
	tmp = llabs(tmp / 2);		/* dist = v^2/2 * |(a3-a1)/(a1*a3)| */

	if(labs(trajectory->s4 - trajectory->s1) <= tmp)
	{	/* Profile_velocity is not reached */
		/* calculate total time */
		tmp = (long long)(trajectory->s4 - trajectory->s1) * (trajectory->a3 - trajectory->a1);
		tmp /= trajectory->a1;
		tmp *= ((long)lcounts_p_s * lcounts_p_s * 2);
		tmp /= trajectory->a3;
		tmp = root(tmp);
		trajectory->t4 = trajectory->t1 + tmp;

		/* calculate intermediate time */
		tmp = (long long)(trajectory->s4 - trajectory->s1) * trajectory->a3;
		tmp /= trajectory->a1;
		tmp *= ((long)lcounts_p_s * lcounts_p_s * 2);
		tmp /= (trajectory->a3 - trajectory->a1);
		tmp = root(tmp);
		trajectory->t2 = trajectory->t1 + tmp;
		trajectory->t3 = trajectory->t2;

		/* calculate intermediate position */
		tmp = trajectory->s4 - trajectory->s1;
		trajectory->s2 = trajectory->s1 + (tmp/2);
		trajectory->s3 = trajectory->s2;

		/* calculate Profile velocity */
		tmp = (long long)((long)(trajectory->t3 - trajectory->t1)) * trajectory->a1;
		tmp = (tmp/lcounts_p_s) + trajectory->v1;
		trajectory->v2 = tmp;
	}
	else
	{
		trajectory->v2 = sign(trajectory->s4 - trajectory->s1) * trajectory->v2;	/* correct the sign of v2 */

		/* set the time to reach constant velocity */
		tmp = (long long)trajectory->v2 * lcounts_p_s;
		trajectory->t2 = trajectory->t1 + (tmp / trajectory->a1);

		/* set the position to reach constant velocity */
		tmp = (long long)trajectory->v2 * trajectory->v2;
		trajectory->s2 = trajectory->s1 + (tmp / (2 * trajectory->a1));	/*  s2 = s1 + 1/2 * v2^2/a1  */

		/* set the position to begin deceleration */
		tmp = (long long)trajectory->v2 * trajectory->v2;
		trajectory->s3 = trajectory->s4 + (tmp / (2 * trajectory->a3)); 		/*  s3 = s4 + 1/2 * v2^2/a3  */

		/* set the time to begin deceleration */
		tmp = (long long)(trajectory->s3 - trajectory->s2) * lcounts_p_s;
		trajectory->t3 = trajectory->t2 + (tmp / trajectory->v2);

		/* set the time to stop */
		tmp = (long long)trajectory->v2 * lcounts_p_s;
		trajectory->t4 = trajectory->t3 - (tmp / trajectory->a3);		/*  t4 = t3 - (v2/a3)  */
	}
}


/*****************************************************************/
/*!	 Calculates a position trajectory to stop the drive at a given deceleration
	\param dec Deceleration in [position units]
	\param trajectory Position trajectory to be created
	\param state Cinematic state of the system
*/
/*****************************************************************/
void pp_stop_trajectory(unsigned long dec, position_trajectory_struct *trajectory, motion_state_struct *state)
{
	unsigned long dec_incs;
	long long stop_length;
	long long stop_time;		/* stop time */
	long long tmp;

	tmp = (long long)minimum(dec, Max_deceleration) * Acceleration_factor_Numerator;
	dec_incs = tmp / Acceleration_factor_Divisor;		/* limited and converted into [incs/s^2] */

	/* set trajectory initial values */
	trajectory->t0 = state->time;
	trajectory->s0 = state->position;
	trajectory->a0 = (-1) * sign(state->velocity) * dec_incs;

	/* calculate the length it will take it to stop */
	stop_length = (long long)state->velocity * state->velocity;
	stop_length = -stop_length / (trajectory->a0 * 2);	/*  stop_length = -1/2 * (v0^2/a0)  (can be negative) */

	/* set the stop time */
	stop_time = (long long)state->velocity * lcounts_p_s;
	stop_time = -stop_time / trajectory->a0;

	trajectory->v0 = state->velocity;
	trajectory->v1 = 0;
	trajectory->v2 = 0;
	trajectory->t1 = trajectory->t0 + stop_time;

	/* set the stop position */
	trajectory->s1 = trajectory->s0 + stop_length;

	/* set the rest of the trajectory */
	trajectory->s2 = trajectory->s1;
	trajectory->s3 = trajectory->s1;
	trajectory->s4 = trajectory->s1;
	trajectory->t2 = trajectory->t1;
	trajectory->t3 = trajectory->t1;
	trajectory->t4 = trajectory->t1;

	/* calculate target in position units */
	//trajectory->target = int2ext_pos(trajectory->s1, Home_offset);		/* [position units] */
}





/*****************************************************************/
/*!	 Calculates a position trajectory with 2 deceleration sections (starting from a velocity above profile_velocity)
	\param trajectory Position trajectory to be created
*/
/*****************************************************************/
void pp_trajectory_2decs(position_trajectory_struct *trajectory)
{
	long long tmp;

	/* correct the sign of v2 */
	trajectory->v2 = sign(trajectory->v1) * trajectory->v2;

	/* set the time to reach constant velocity */
	tmp = (long long)(trajectory->v2 - trajectory->v1) * lcounts_p_s;
	trajectory->t2 = trajectory->t1 + (tmp / trajectory->a1);

	/* set the position to reach constant velocity */
	/* s2 = s1 + (v2-v1)/a1 * (v1+v2)/2 */
	tmp = (long long)(trajectory->v2 - trajectory->v1) * (trajectory->v1 + trajectory->v2);
	trajectory->s2 = trajectory->s1 + (tmp / (2 * trajectory->a1));

	/* set the position to begin deceleration */
	tmp = (long long)trajectory->v2 * trajectory->v2;
	trajectory->s3 = trajectory->s4 + (tmp / (2 * trajectory->a3)); 		/*  s3 = s4 + 1/2 * v2^2/a3  */

	/* set the time to begin deceleration */
	tmp = (long long)(trajectory->s3 - trajectory->s2) * lcounts_p_s;
	trajectory->t3 = trajectory->t2 + (tmp / trajectory->v2);

	/* set the time to stop */
	tmp = (long long)trajectory->v2 * lcounts_p_s;
	trajectory->t4 = trajectory->t3 - (tmp / trajectory->a3);		/*  t4 = t3 - (v2/a3)  */
}


/*****************************************************************/
/*!	 Check if target postion, velocity and accelerations meet their limits
	\param new_target New target position [position units]
	\param limited_target Returns new target position after being limitated [position units]
	\param target_pos Returns new target position after being limitated [increments]
	\param vel Receives a velocity in [velocity units] that is limitated and returned converted into [inc/s]
	\param acc Receives an acceleration in [acceleration units] that is limitated and returned converted into [inc/s^2]
	\param dec Receives a deceleration in [acceleration units] that is limitated and returned converted into [inc/s^2]
*/
/*****************************************************************/
void pp_limits(long new_target, long *limited_target, long *target_pos, long *vel, long *acc, long *dec)
{
	long long tmp;

	/* calculate limited accelerations in [inc/s^2] */
	tmp = (long long)minimum(*acc, Max_acceleration) * Acceleration_factor_Numerator;
	tmp /= Acceleration_factor_Divisor;
	*acc = tmp;

	tmp = (long long)minimum(*dec, Max_deceleration) * Acceleration_factor_Numerator;
	tmp /= Acceleration_factor_Divisor;
	*dec = tmp;

	/* calculate limited target position in position units */
	if(new_target > (long)Software_position_limit_Max_position_limit) tmp = (long)Software_position_limit_Max_position_limit;
	else if (new_target < (long)Software_position_limit_Min_position_limit) tmp = (long)Software_position_limit_Min_position_limit;
	else tmp = new_target;
	*limited_target = tmp;

	/* calculate limited target position in [inc] */
	*target_pos = ext2int_pos(tmp, Home_offset);

	/* calculate limited profile velocity in [inc/s] */
	tmp = (long long)Max_motor_speed * Velocity_factor_1_Numerator;
	tmp /= Velocity_factor_1_Divisor;					/* Max_motor_speed limit */
	tmp = minimum(tmp,Max_profile_velocity);			/* minimum of Max_motor_speed and Max_profile_velocity */
	tmp = minimum(*vel, tmp);
	*vel = labs( ext2int_vel( tmp ) );
}


/*****************************************************************/
/*!	 Sets Profile Position Mode status flags (Target Reached and Following Error)
	\param position Current position [position units]
	\param demand Demanded position for this control cycle [position units]
	\param target Target position [position units]
	\param state Cynematic state (position, velocity and time)
	\param reset If reset = 1, timers will be restarted
*/
/*****************************************************************/
void pp_status_flags(long int position, long int demand, long int target, long int halt, motion_state_struct *state, int reset)
{
	static long long p_error_started = 0, p_reached_started = 0, p_halt_reached_started = 0;
	unsigned long long tmp;
	long following_error;

	if( reset )
	{
		p_error_started = 0;
		p_reached_started = 0;
		p_halt_reached_started = 0;
	}

	/* Check if target position reached */
	if(labs(target - position) <= Position_window)
	{
		if(!p_reached_started) p_reached_started = state->time;		/* Start time counting if not started */
		tmp = (long long)(state->time - p_reached_started) * 1000;
		tmp /= lcounts_p_s;
		if(tmp > Position_window_time)
		{
			if( pp_following_target )
				Device_status_word |= TARGET_REACHED_MASKBIT;

			/* if there is a non-immediate target, StatusWord is not affected when applying the next target
				(SET_POINT_ACK is cleared and set in the same cycle an StatusWord is not set)
				maybe setting SET_POINT_ACK for only 1 cycle and clearing after could work */
			if(pp_trajectory.next_target_active && !(Device_control_word & HALT_MASKBIT))
			{
				pp_trajectory.next_target_active = 0;
				pp_trajectory_generator(pp_trajectory.next_target, &pp_trajectory, state);
				Device_status_word |= SET_POINT_ACKNOWLEDGE_MASKBIT;
				Device_status_word &= ~TARGET_REACHED_MASKBIT;			/* Target reached bit = 0 */
				pp_following_target = 1;
				send_SWord = 1;
				p_error_started = 0;
				p_reached_started = 0;
			}
		}
		else
		{
			Device_status_word &= ~TARGET_REACHED_MASKBIT;			/* Target reached bit = 0 */
		}
	}
	else
	{
		p_reached_started = 0;			/* stop time counting */
		Device_status_word &= ~TARGET_REACHED_MASKBIT;			/* Target reached bit = 0 */
	}

	/* Check if following error is too big */
	following_error = demand - position;
	if(labs(following_error) > Following_error_window)
	{
		if(!p_error_started)
		{
			p_error_started = state->time;		/* Start time counting if not started */
		}
		tmp = (long long)(state->time - p_error_started) * 1000;
		tmp /= lcounts_p_s;
		if(tmp > Following_error_time_out)
		{
			if( pp_following_target )
			{
				Device_status_word |= FOLLOWING_ERROR_MASKBIT;

				/* set the position following error */
				if(!(isFaultActive(FAULT_POS_FOLLOWING)))
				{
					_ERRORmessage(0x8611, 0x20, 0x0000, "position following error", 0, 0);
					setFault(FAULT_POS_FOLLOWING);
				}
				QueueFault(FAULT_POS_FOLLOWING);
				//disable the checking because the system enters in OPERATION_ENABLE before mode is initialized
				pp_following_target = 0;
			}
		}
		else
		{
			Device_status_word &= ~FOLLOWING_ERROR_MASKBIT;
			DeQueueFault(FAULT_POS_FOLLOWING);
		}
	}
	else
	{
		p_error_started = 0;			/* stop time counting */
		Device_status_word &= ~FOLLOWING_ERROR_MASKBIT;			/* Following error bit = 0 */
		DeQueueFault(FAULT_POS_FOLLOWING);
	}
	/* Check if s4 is within target reached window */
	if(labs(halt - position) <= Position_window)
	{
		if(!p_halt_reached_started) p_halt_reached_started = state->time;		/* Start time counting if not started */
		tmp = (long long)(state->time - p_halt_reached_started) * 1000;
		tmp /= lcounts_p_s;
		if(tmp > Position_window_time)
		{
			if( pp_following_target )
				Device_status_word |= HALT_REACHED_MASKBIT;
		}
		else
		{
			Device_status_word &= ~HALT_REACHED_MASKBIT;			/* Target reached bit = 0 */
		}
	}
	else
	{
		p_halt_reached_started = 0;			/* stop time counting */
		Device_status_word &= ~HALT_REACHED_MASKBIT;			/* Target reached bit = 0 */
	}
}


/*****************************************************************/
/*!	 Function that makes necessary initializations when entering profile position mode
	\param state Cinematic state of the system
*/
/*****************************************************************/
void pp_initialize(motion_state_struct *state)
{
	long target;

	while(MBX_pend(&pp_newtarget_mbox, &target, 0));	/* empty pp_newtarget_mbox */
	while(MBX_pend(&pp_nexttarget_mbox, &target, 0));	/* enpty pp_nextarget_mbox */

	/* set stop trajectory */
	pp_stop_trajectory(Profile_deceleration, &pp_trajectory, state);
	pp_trajectory.next_target_active = 0;		/* disable next target (if any) */

	/* reset mode specific bits of Statusword */
	Device_status_word &= ~(TARGET_REACHED_MASKBIT | FOLLOWING_ERROR_MASKBIT | HALT_REACHED_MASKBIT);
	send_SWord = 1;

	/* initialize variables */
	pp_following_target = 0;

	/* Reset current error BLAC current control*/
	Ui_d=0;
	Up_d=0;
	SatErr_d=0;
	Ui_q=0;
	Up_q=0;
	SatErr_q=0;

	/* initialize PID */
	position_control_pid_mms.Err = 0;
	position_control_pid_mms.Up = 0;
	position_control_pid_mms.Ui = 0;
	position_control_pid_mms.Ud = 0;
	position_control_pid_mms.OutPreSat = 0;
	position_control_pid_mms.Out = 0;
	position_control_pid_mms.SatErr = 0;
	position_control_pid_mms.Err1 = 0;
	position_control_pid_mms.OutMax = MAX_DUTY_CYCLE;
	position_control_pid_mms.OutMin = MIN_DUTY_CYCLE;

	position_control_pid_mms.Period = _IQ(0.01);
	position_control_pid_mms.Frequency = 100;

	pos_vel_control_pid_mms.Err = 0;
	pos_vel_control_pid_mms.Up = 0;
	pos_vel_control_pid_mms.Ui = 0;
	pos_vel_control_pid_mms.Ud = 0;
	pos_vel_control_pid_mms.OutPreSat = 0;
	pos_vel_control_pid_mms.Out = 0;
	pos_vel_control_pid_mms.SatErr = 0;
	pos_vel_control_pid_mms.Err1 = 0;
	pos_vel_control_pid_mms.OutMax = MAX_DUTY_CYCLE;
	pos_vel_control_pid_mms.OutMin = MIN_DUTY_CYCLE;

	pos_vel_control_pid_mms.Period = _IQ(0.001);
	pos_vel_control_pid_mms.Frequency = 1000;

	init_current_controller_dc_bldc();

	pp_set_pid_tunning();

	DINT;
	p_current_limit = &current_limit;	/* set current limit to use */
	EINT;

	ATM_seti(&ready_to_power_on, 1);
}


/*****************************************************************/
/*!	 Calculates the velocity demand for current control cycle
	\param trajectory Position trajectory being followed
	\param current_time Current time
	\return Velocity demand
*/
/*****************************************************************/
long pp_trajectory_vel(position_trajectory_struct *trajectory, long long current_time)
{
	long long tmp;
	long ret;

	/* It depends on the motion profile type (by now only linear ramp mode used) */
	if(current_time >= trajectory->t4)
	{
		/* v = v4 */
		ret = trajectory->v4;
	}
	else if(current_time >= trajectory->t3)
	{
		/* v = v2 + a3 * (t - t3) */
		tmp = (long long)((long)(current_time - trajectory->t3)) * trajectory->a3;
		tmp = (tmp/lcounts_p_s) + trajectory->v2;
		ret = tmp;
	}
	else if(current_time >= trajectory->t2)
	{
		/* v = v2 */
		ret = trajectory->v2;
	}
	else if(current_time >= trajectory->t1)
	{
		/* v =  v1 + a1 * (t - t1) */
		tmp = (long long)((long)(current_time - trajectory->t1)) * trajectory->a1;
		tmp = (tmp/lcounts_p_s) + trajectory->v1;
		ret = tmp;
	}
	else
	{
		/* v = v0 + a0 * (t - t0) */
		tmp = (long long)((long)(current_time - trajectory->t0)) * trajectory->a0;
		tmp = (tmp/lcounts_p_s) + trajectory->v0;
		ret = tmp;
	}
	return ret;
}


void pp_trajectory_set_stop_point( unsigned long dec, position_trajectory_struct *trajectory, motion_state_struct *state )
{
	unsigned long dec_incs;
	long target;
	long vel;
	long long stop_length;
	long long tmp;
	long long sim_time;

	//Already braked or braking
	if(state->time >= trajectory->t3)
		return;

	tmp = (long long)minimum(dec, Max_deceleration) * Acceleration_factor_Numerator;
	dec_incs = tmp / Acceleration_factor_Divisor;		/* limited and converted into [incs/s^2] */

	trajectory->a3 = sign(trajectory->a3) * dec_incs;

	/* The profile velocity is used instead of current one,
	 * because the current one could have noise (gearbox gap) */
	//vel = state->velocity;
	vel = pp_trajectory_vel(trajectory, state->time);

	trajectory->v2 = vel;
	sim_time = state->time + (5000/lcounts_p_s);
	trajectory->s2 = pp_trajectory_point(trajectory, sim_time);
	trajectory->t2 = state->time;

	sim_time = state->time + (10000/lcounts_p_s);
	trajectory->s3 = pp_trajectory_point(trajectory, sim_time);
	trajectory->t3 = sim_time;
	stop_length = (long long)vel * vel;
	stop_length = stop_length / (trajectory->a3 * 2);	/*  stop_length = -1/2 * (v0^2/a0)  (can be negative) */
	stop_length = sign(vel)*labs(stop_length);
	target = trajectory->s3 + stop_length;

	tmp = (long long)trajectory->v2 * lcounts_p_s;
	trajectory->t4 = trajectory->t3 - (tmp / trajectory->a3);		/*  t4 = t3 - (v2/a3)  */
	trajectory->s4 = target;
}


void pp_trajectory_reset( position_trajectory_struct *trajectory, motion_state_struct *state )
{
	trajectory->t4 = state->time;
	trajectory->s4 = state->position;
}


int set_position_control_dataset( void )
{
	int i;
	int set = Position_control_dataset[0];
	set=0;

	if( Sensors_configuration_active )
	{
		for(i=0;i<5;i++)
		{
			Position_control_parameter_set[i] = Position_control_dataset[1+(set*6)+i];
		}
		Position_control_margin = (long long)Position_control_dataset[1+(set*6)+5];
	}
	return 0;
}

int set_position_velocity_control_dataset( void )
{
	int i;
	int set = Position_Velocity_control_dataset[0];
	set=0;

	if( Sensors_configuration_active )
	{
		for(i=0;i<5;i++)
		{
			Position_Velocity_control_parameter_set[i] = Position_Velocity_control_dataset[1+(set*6)+i];
		}
	}
	return 0;
}


int set_position_current_control_dataset( void )
{
	int i;
	int set = Position_Current_control_dataset[0];
	set=0;

	if( Sensors_configuration_active )
	{
		for(i=0;i<5;i++)
		{
			Position_Current_control_parameter_set[i] = Position_Current_control_dataset[1+(set*6)+i];
		}
	}
	return 0;
}
