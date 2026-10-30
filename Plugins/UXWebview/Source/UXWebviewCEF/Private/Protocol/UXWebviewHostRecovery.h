#pragma once

#include "CoreMinimal.h"

/** Where the shared host process is in its lifecycle. */
enum class EUXWebviewHostRecoveryState : uint8
{
	Idle,
	Starting,
	Connected,
	Recovering,
	Failed
};

/** What the owner of the state machine must do next. */
enum class EUXWebviewHostRecoveryAction : uint8
{
	None,
	Relaunch,
	GiveUp
};

struct FUXWebviewHostRecoveryConfig
{
	/** Relaunch attempts allowed after the first launch. */
	int32 MaxRestarts = 3;
	double RestartDelaySeconds = 1.0;
	/** Time the host has to accept the IPC connection; cold starts on Windows
	 *  can take a while when the antivirus scans the CEF runtime, and CEF init
	 *  competes with shader compilation right after game start. */
	double StartTimeoutSeconds = 60.0;
	/** PONG deadline while connected. */
	double HeartbeatTimeoutSeconds = 10.0;
	/** A connection that survives this long clears the restart budget. */
	double StableSeconds = 10.0;
};

/**
 * Host process policy without any IPC or process calls, so it can be tested
 * with a fake clock. Times are seconds since an arbitrary epoch.
 */
class FUXWebviewHostRecovery
{
public:
	explicit FUXWebviewHostRecovery(const FUXWebviewHostRecoveryConfig& InConfig = FUXWebviewHostRecoveryConfig())
		: Config(InConfig)
	{
	}

	/** Called right after the process was spawned. */
	void OnLaunched(double NowSeconds)
	{
		if (State == EUXWebviewHostRecoveryState::Failed
			|| State == EUXWebviewHostRecoveryState::Connected)
		{
			return;
		}

		State = EUXWebviewHostRecoveryState::Starting;
		StartedSeconds = NowSeconds;
		LastPongSeconds = 0.0;
	}

	/** Called after HELLO_ACK proved the host speaks our protocol. */
	void OnConnected(double NowSeconds)
	{
		State = EUXWebviewHostRecoveryState::Connected;
		ConnectedSeconds = NowSeconds;
		LastPongSeconds = NowSeconds;
	}

	/** Called when the process exits, the pipe closes or the heartbeat dies. */
	void OnExited(double NowSeconds)
	{
		switch (State)
		{
		case EUXWebviewHostRecoveryState::Starting:
		case EUXWebviewHostRecoveryState::Connected:
			ScheduleRestart(NowSeconds);
			break;
		case EUXWebviewHostRecoveryState::Failed:
		case EUXWebviewHostRecoveryState::Idle:
		case EUXWebviewHostRecoveryState::Recovering:
			break;
		}
	}

	void OnHeartbeatPong(double NowSeconds)
	{
		LastPongSeconds = NowSeconds;
	}

	/** Advances timers; returns the action the caller must perform. */
	EUXWebviewHostRecoveryAction Tick(double NowSeconds)
	{
		switch (State)
		{
		case EUXWebviewHostRecoveryState::Starting:
			if (NowSeconds - StartedSeconds >= Config.StartTimeoutSeconds)
			{
				ScheduleRestart(NowSeconds);
			}
			break;
		case EUXWebviewHostRecoveryState::Connected:
		{
			if (LastPongSeconds > 0.0
				&& NowSeconds - LastPongSeconds >= Config.HeartbeatTimeoutSeconds)
			{
				ScheduleRestart(NowSeconds);
			}
			else if (RestartCount > 0 && NowSeconds - ConnectedSeconds >= Config.StableSeconds)
			{
				RestartCount = 0;
			}
			break;
		}
		case EUXWebviewHostRecoveryState::Recovering:
			if (NowSeconds >= NextRestartSeconds)
			{
				State = EUXWebviewHostRecoveryState::Starting;
				StartedSeconds = NowSeconds;
				LastPongSeconds = 0.0;
				return EUXWebviewHostRecoveryAction::Relaunch;
			}
			break;
		case EUXWebviewHostRecoveryState::Failed:
			if (!bFailureReported)
			{
				bFailureReported = true;
				return EUXWebviewHostRecoveryAction::GiveUp;
			}
			break;
		default:
			break;
		}
		return EUXWebviewHostRecoveryAction::None;
	}

	EUXWebviewHostRecoveryState GetState() const { return State; }
	int32 GetRestartCount() const { return RestartCount; }
	const FUXWebviewHostRecoveryConfig& GetConfig() const { return Config; }

	bool IsRecovering() const { return State == EUXWebviewHostRecoveryState::Recovering; }
	bool IsFailed() const { return State == EUXWebviewHostRecoveryState::Failed; }

	/** Clears every timer, e.g. when the last instance is gone. */
	void Reset()
	{
		State = EUXWebviewHostRecoveryState::Idle;
		RestartCount = 0;
		StartedSeconds = 0.0;
		ConnectedSeconds = 0.0;
		LastPongSeconds = 0.0;
		NextRestartSeconds = 0.0;
		bFailureReported = false;
	}

private:
	void ScheduleRestart(double NowSeconds)
	{
		if (RestartCount >= Config.MaxRestarts)
		{
			State = EUXWebviewHostRecoveryState::Failed;
			bFailureReported = false;
			return;
		}

		++RestartCount;
		State = EUXWebviewHostRecoveryState::Recovering;
		NextRestartSeconds = NowSeconds + Config.RestartDelaySeconds;
	}

	FUXWebviewHostRecoveryConfig Config;
	EUXWebviewHostRecoveryState State = EUXWebviewHostRecoveryState::Idle;
	int32 RestartCount = 0;
	double StartedSeconds = 0.0;
	double ConnectedSeconds = 0.0;
	double LastPongSeconds = 0.0;
	double NextRestartSeconds = 0.0;
	bool bFailureReported = false;
};
