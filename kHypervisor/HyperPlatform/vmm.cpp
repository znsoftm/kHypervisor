// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Copyright (c) 2016-2017, KelvinChan. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements VMM functions.

#include "vmm.h"
#include <intrin.h>
#include "asm.h"
#include "common.h"
#include "ept.h"
#include "log.h"
#include "util.h"
#include "performance.h"
#include "vmcs.h"
#include "vmx.h"
#pragma warning(disable: 4505)
extern "C" {
	////////////////////////////////////////////////////////////////////////////////
	//
	// macro utilities
	//

	////////////////////////////////////////////////////////////////////////////////
	//
	// constants and macros
	//
	BOOLEAN IsEmulateVMExit = FALSE;
	// Whether VM-exit recording is enabled
	static const long kVmmpEnableRecordVmExit = true;

	// How many events should be recorded per a processor
	static const long kVmmpNumberOfRecords = 100;

	// How many processors are supported for recording
	static const long kVmmpNumberOfProcessors = 4;

	////////////////////////////////////////////////////////////////////////////////
	//
	// types
	//
	// Represents raw structure of stack of VMM when VmmVmExitHandler() is called
	struct VmmInitialStack {
		GpRegisters gp_regs;
		ULONG_PTR reserved;
		ProcessorData *processor_data;

	};

#pragma pack(8)
	struct GuestContext {
		union {
			VmmInitialStack *stack;
			GpRegisters *gp_regs;
		};
		FlagRegister flag_reg;
		ULONG_PTR ip;
		ULONG_PTR cr8;
		KIRQL irql;
		bool vm_continue;
	};
#pragma pack()

#if defined(_AMD64_)
	static_assert(sizeof(GuestContext) == 40, "Size check");
#else
	static_assert(sizeof(GuestContext) == 20, "Size check");
#endif




	// Context at the moment of vmexit
	struct VmExitHistory {
		GpRegisters gp_regs;
		ULONG_PTR ip;
		VmExitInformation exit_reason;
		ULONG_PTR exit_qualification;
		ULONG_PTR instruction_info;
		VmExitInterruptionInformationField exception_infomation_field;
	};

	////////////////////////////////////////////////////////////////////////////////
	//
	// prototypes
	//

	bool __stdcall VmmVmExitHandler(_Inout_ VmmInitialStack *stack);

	DECLSPEC_NORETURN void __stdcall VmmVmxFailureHandler(
		_Inout_ AllRegisters *all_regs);

	static void VmmpHandleVmExit(_Inout_ GuestContext *guest_context);

	DECLSPEC_NORETURN static void VmmpHandleTripleFault(
		_Inout_ GuestContext *guest_context);

	DECLSPEC_NORETURN static void VmmpHandleUnexpectedExit(
		_Inout_ GuestContext *guest_context);

	static void VmmpHandleMonitorTrap(_Inout_ GuestContext *guest_context);

	static void VmmpHandleException(_Inout_ GuestContext *guest_context);

	static void VmmpHandleCpuid(_Inout_ GuestContext *guest_context);

	static void VmmpHandleRdtsc(_Inout_ GuestContext *guest_context);

	static void VmmpHandleRdtscp(_Inout_ GuestContext *guest_context);

	static void VmmpHandleXsetbv(_Inout_ GuestContext *guest_context);

	static void VmmpHandleMsrReadAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleMsrWriteAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleMsrAccess(_Inout_ GuestContext *guest_context,
		_In_ bool read_access);

	static void VmmpHandleGdtrOrIdtrAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleLdtrOrTrAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleDrAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleIoPort(_Inout_ GuestContext *guest_context);

	static void VmmpHandleCrAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleVmx(_Inout_ GuestContext *guest_context);

	static void VmmpHandleVmCall(_Inout_ GuestContext *guest_context);

	static void VmmpHandleInvalidateInternalCaches(
		_Inout_ GuestContext *guest_context);

	static void VmmpHandleInvalidateTlbEntry(_Inout_ GuestContext *guest_context);

	static void VmmpHandleEptViolation(_Inout_ GuestContext *guest_context);

	static void VmmpHandleEptMisconfig(_Inout_ GuestContext *guest_context);

	ULONG_PTR *VmmpSelectRegister(_In_ ULONG index,
		_In_ GuestContext *guest_context);

	static void VmmpDumpGuestSelectors();

	static void VmmpAdjustGuestInstructionPointer(_In_ GuestContext *guest_context);

	static void VmmpIoWrapper(_In_ bool to_memory, _In_ bool is_string,
		_In_ SIZE_T size_of_access, _In_ unsigned short port,
		_Inout_ void *address, _In_ unsigned long count);

	static void VmmpSaveExtendedProcessorState(_Inout_ GuestContext *guest_context);

	static void VmmpRestoreExtendedProcessorState(_In_ GuestContext *guest_context);

	static void VmmpIndicateSuccessfulVmcall(_In_ GuestContext *guest_context);

	static void VmmpIndicateUnsuccessfulVmcall(_In_ GuestContext *guest_context);

	static void VmmpHandleVmCallTermination(_In_ GuestContext *guest_context,
		_Inout_ void *context);

	static UCHAR VmmpGetGuestCpl();

	static void VmmpInjectInterruption(_In_ InterruptionType interruption_type,
		_In_ InterruptionVector vector,
		_In_ bool deliver_error_code,
		_In_ ULONG32 error_code);

	////////////////////////////////////////////////////////////////////////////////
	//
	// variables
	// 
	// Those variables are all for diagnostic purpose
	static ULONG g_vmmp_next_history_index[kVmmpNumberOfProcessors];
	static VmExitHistory g_vmmp_vm_exit_history[kVmmpNumberOfProcessors]
		[kVmmpNumberOfRecords];
	extern VOID VMSucceed(FlagRegister* reg);
	////////////////////////////////////////////////////////////////////////////////
	//
	// implementations


	//----------------------------------------------------------------------------------------------------------------//
	GpRegisters* GetGpReg(GuestContext* guest_context)
	{
		return	guest_context->gp_regs;
	}

	//----------------------------------------------------------------------------------------------------------------//
	FlagRegister* GetFlagReg(GuestContext* guest_context)
	{
		return &guest_context->flag_reg;
	}

	//----------------------------------------------------------------------------------------------------------------//
	KIRQL GetGuestIrql(GuestContext* guest_context)
	{
		return guest_context->irql;
	}


	//----------------------------------------------------------------------------------------------------------------//
	ULONG_PTR GetGuestCr8(GuestContext* guest_context)
	{
		return guest_context->cr8;
	}

	//----------------------------------------------------------------------------------------------------------------//
	ULONG GetvCpuMode(GuestContext* guest_context)
	{
		return guest_context->stack->processor_data->CpuMode;
	}

	//----------------------------------------------------------------------------------------------------------------//
	VOID SetvCpuMode(GuestContext* guest_context, CPU_MODE CpuMode)
	{
		guest_context->stack->processor_data->CpuMode = CpuMode;
	}

	//----------------------------------------------------------------------------------------------------------------//
	VOID EnterVmxMode(GuestContext* guest_context)
	{
		SetvCpuMode(guest_context, VmxMode);
	}

	//----------------------------------------------------------------------------------------------------------------//
	VOID LeaveVmxMode(GuestContext* guest_context)
	{
		SetvCpuMode(guest_context, ProtectedMode);
	}

	//----------------------------------------------------------------------------------------------------------------//
	VCPUVMX* GetVcpuVmx(GuestContext* guest_context)
	{
		return guest_context->stack->processor_data->vcpu_vmx;
	}

	//----------------------------------------------------------------------------------------------------------------//
	VOID SetvCpuVmx(GuestContext* guest_context, VCPUVMX* VCPUVMX)
	{
		guest_context->stack->processor_data->vcpu_vmx = VCPUVMX;
	}

	//----------------------------------------------------------------------------------------------------------------//

	// A high level VMX handler called from AsmVmExitHandler().
	// Return true for vmresume, or return false for vmxoff.
#pragma warning(push)
#pragma warning(disable : 28167)
	_Use_decl_annotations_ bool __stdcall VmmVmExitHandler(VmmInitialStack *stack)
	{

		// Save guest's context and raise IRQL as quick as possible
		const auto guest_irql = KeGetCurrentIrql();
		const auto guest_cr8 = IsX64() ? __readcr8() : 0;
		if (guest_irql < DISPATCH_LEVEL) {
			KeRaiseIrqlToDpcLevel();
		}
		NT_ASSERT(stack->reserved == MAXULONG_PTR);

		// Capture the current guest state
		GuestContext guest_context = { stack,
			UtilVmRead(VmcsField::kGuestRflags),
			UtilVmRead(VmcsField::kGuestRip),
			guest_cr8,
			guest_irql,
			true };

		guest_context.gp_regs->sp = UtilVmRead(VmcsField::kGuestRsp);

		VmmpSaveExtendedProcessorState(&guest_context);

		// Dispatch the current VM-exit event
		VmmpHandleVmExit(&guest_context);

		VmmpRestoreExtendedProcessorState(&guest_context);

		// See: Guidelines for Use of the INVVPID Instruction, and Guidelines for Use
		// of the INVEPT Instruction
		if (!guest_context.vm_continue)
		{
			UtilInveptGlobal();
			UtilInvvpidAllContext();
		}

		// Restore guest's context
		if (guest_context.irql < DISPATCH_LEVEL)//&& !IsEmulateVMExit)
		{
			KeLowerIrql(guest_context.irql);
		}

		// Apply possibly updated CR8 by the handler
		if (IsX64()) //&& !IsEmulateVMExit) {
		{
			__writecr8(guest_context.cr8);
		}
		return guest_context.vm_continue;
	}
#pragma warning(pop)
	_Use_decl_annotations_ static void VmmpHandleVmExitForL1(VmExitInformation exit_reason, GuestContext *guest_context)
	{
		switch (exit_reason.fields.reason)
		{
		case VmxExitReason::kExceptionOrNmi:
			VmmpHandleException(guest_context);
			break;
		case VmxExitReason::kTripleFault:
			VmmpHandleTripleFault(guest_context);
			break;
		case VmxExitReason::kCpuid:
			VmmpHandleCpuid(guest_context);
			break;
		case VmxExitReason::kInvd:
			VmmpHandleInvalidateInternalCaches(guest_context);
			break;
		case VmxExitReason::kInvlpg:
			VmmpHandleInvalidateTlbEntry(guest_context);
			break;
		case VmxExitReason::kRdtsc:
			VmmpHandleRdtsc(guest_context);
			break;
		case VmxExitReason::kCrAccess:
			VmmpHandleCrAccess(guest_context);
			break;
		case VmxExitReason::kDrAccess:
			VmmpHandleDrAccess(guest_context);
			break;
		case VmxExitReason::kIoInstruction:
			VmmpHandleIoPort(guest_context);
			break;
		case VmxExitReason::kMsrRead:
			VmmpHandleMsrReadAccess(guest_context);
			break;
		case VmxExitReason::kMsrWrite:
			VmmpHandleMsrWriteAccess(guest_context);
			break;
		case VmxExitReason::kMonitorTrapFlag:
			VmmpHandleMonitorTrap(guest_context);
			break;
		case VmxExitReason::kGdtrOrIdtrAccess:
			VmmpHandleGdtrOrIdtrAccess(guest_context);
			break;
		case VmxExitReason::kLdtrOrTrAccess:
			VmmpHandleLdtrOrTrAccess(guest_context);
			break;
		case VmxExitReason::kEptViolation:
			VmmpHandleEptViolation(guest_context);
			break;
		case VmxExitReason::kEptMisconfig:
			VmmpHandleEptMisconfig(guest_context);
			break;
		case VmxExitReason::kVmcall:
			VmmpHandleVmCall(guest_context);
			break;
		case VmxExitReason::kVmclear:
		case VmxExitReason::kVmlaunch:
		case VmxExitReason::kVmptrld:
		case VmxExitReason::kVmptrst:
		case VmxExitReason::kVmread:
		case VmxExitReason::kVmresume:
		case VmxExitReason::kVmwrite:
		case VmxExitReason::kVmoff:
		case VmxExitReason::kVmon:
			VmmpHandleVmx(guest_context);
			break;
		case VmxExitReason::kRdtscp:
			VmmpHandleRdtscp(guest_context);
			break;
		case VmxExitReason::kXsetbv:
			VmmpHandleXsetbv(guest_context);
			break;
		default:
			VmmpHandleUnexpectedExit(guest_context);
			break;
		}
	}
	//-----------------------------------------------------------------------------------------------------------------------//
	_Use_decl_annotations_ static BOOLEAN VmmpHandleExceptionForL2(
		_In_ VmExitInformation exit_reason,
		_In_ GuestContext *guest_context
	)
	{
		BOOLEAN status = FALSE;
		VmExitInterruptionInformationField 	exception = { static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo)) };
		if (static_cast<InterruptionVector>(exception.fields.vector) == InterruptionVector::kBreakpointException)
		{
			HYPERPLATFORM_COMMON_DBG_BREAK();
			HYPERPLATFORM_LOG_DEBUG_SAFE("Nested VMExit INT 3 Ready !!!! \r\n");
			if (status = VMExitEmulationTest(GetVcpuVmx(guest_context), exit_reason, guest_context))
			{
				IsEmulateVMExit = true;
			}
		}
		return status;
	}
	//-----------------------------------------------------------------------------------------------------------------------//
	_Use_decl_annotations_ static BOOLEAN VmmpHandleVmcallForL2(
		_In_ VmExitInformation exit_reason,
		_In_ GuestContext *guest_context
	)
	{ 
		BOOLEAN status = false;
			HYPERPLATFORM_COMMON_DBG_BREAK();
			HYPERPLATFORM_LOG_DEBUG_SAFE("Nested VMExit - vmcall Ready !!!! \r\n");
			if (status = VMExitEmulationTest(GetVcpuVmx(guest_context), exit_reason, guest_context))
			{
				IsEmulateVMExit = true;
			}
		return status;
	}

	//-----------------------------------------------------------------------------------------------------------------------//
	_Use_decl_annotations_ static void VmmpHandleCpuidForL2(
		_In_ VmExitInformation exit_reason,
		_In_ GuestContext *guest_context
	)
	{
		if (VMExitEmulationTest(GetVcpuVmx(guest_context), exit_reason, guest_context))
		{
			IsEmulateVMExit = true;
		}
	}

	//-----------------------------------------------------------------------------------------------------------------------//
	_Use_decl_annotations_ static BOOLEAN VmmpHandleVmExitForL2(
		_In_ VmExitInformation exit_reason,
		_In_ GuestContext *guest_context
	)
	{
		BOOLEAN IsHandled = FALSE;
		switch (exit_reason.fields.reason)
		{
		case VmxExitReason::kCpuid:
			///	VmmpHandleCpuidForL2(exit_reason, guest_context);
			///	IsHandled = TRUE;
			break;
		case VmxExitReason::kExceptionOrNmi:
			IsHandled = VmmpHandleExceptionForL2(exit_reason, guest_context);
			break;
		case VmxExitReason::kTripleFault:
			break;
		case VmxExitReason::kInvd:
			break;
		case VmxExitReason::kInvlpg:
			break;
		case VmxExitReason::kRdtsc:
			break;
		case VmxExitReason::kCrAccess:
			break;
		case VmxExitReason::kDrAccess:
			break;
		case VmxExitReason::kIoInstruction:
			break;
		case VmxExitReason::kMsrRead:
			break;
		case VmxExitReason::kMsrWrite:
			break;
		case VmxExitReason::kMonitorTrapFlag:
			break;
		case VmxExitReason::kGdtrOrIdtrAccess:
			break;
		case VmxExitReason::kLdtrOrTrAccess:
			break;
		case VmxExitReason::kEptViolation:
			break;
		case VmxExitReason::kEptMisconfig:
			break;
		case VmxExitReason::kVmcall:
			IsHandled = VmmpHandleVmcallForL2(exit_reason, guest_context);
			break;
		case VmxExitReason::kVmclear:
		case VmxExitReason::kVmlaunch:
		case VmxExitReason::kVmptrld:
		case VmxExitReason::kVmptrst:
		case VmxExitReason::kVmread:
		case VmxExitReason::kVmresume:
		case VmxExitReason::kVmwrite:
		case VmxExitReason::kVmoff:
		case VmxExitReason::kVmon:
			break;
		case VmxExitReason::kRdtscp:
			break;
		case VmxExitReason::kXsetbv:
			break;
		default:
			break;
		}
		return IsHandled;
	}

	//---------------------------------------------------------------------------------------------------------------------//
	// Dispatches VM-exit to a corresponding handler
	_Use_decl_annotations_ static void VmmpHandleVmExit(GuestContext *guest_context)
	{
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE(); 
		const VmExitInformation exit_reason = { static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };

		if (kVmmpEnableRecordVmExit)
		{	 // Save them for ease of trouble shooting
			const auto processor = KeGetCurrentProcessorNumberEx(nullptr);
			auto &index = g_vmmp_next_history_index[processor];
			auto &history = g_vmmp_vm_exit_history[processor][index];

			history.gp_regs = *guest_context->gp_regs;
			history.ip = guest_context->ip;
			history.exit_reason = exit_reason;
			history.exit_qualification = UtilVmRead(VmcsField::kExitQualification);
			history.instruction_info = UtilVmRead(VmcsField::kVmxInstructionInfo);
			history.exception_infomation_field = { (ULONG32)UtilVmRead(VmcsField::kVmExitIntrInfo) };
			if (++index == kVmmpNumberOfRecords)
			{
				index = 0;
			}
		}
		
		IsEmulateVMExit = FALSE;
		//after vmxon emulation
		if (GetvCpuMode(guest_context) == VmxMode)
		{
			//after vmptrld emulation
			//after vmlaunch / vmresume emulation		
			ULONG64 vmcs_pa = 0;
			VCPUVMX* vCPU = GetVcpuVmx(guest_context);

			__vmx_vmptrst(&vmcs_pa);
			
			if (GetVmxMode(vCPU) == GuestMode && vCPU->vmcs02_pa == vmcs_pa)
			{
				if (!VmmpHandleVmExitForL2(exit_reason, guest_context))
				{
					VmmpHandleVmExitForL1(exit_reason, guest_context);
				}
				else 
				{
					//guest_context->stack->processor_data->vcpu_vmx->guest_irql = guest_context->irql;
				}
			}
			else
			{
				VmmpHandleVmExitForL1(exit_reason, guest_context);
			}
		}
		else
		{
			if (exit_reason.fields.reason == VmxExitReason::kExceptionOrNmi)
			{
				HYPERPLATFORM_LOG_DEBUG("reason: %x \r\n", exit_reason.fields.reason);
			}
			VmmpHandleVmExitForL1(exit_reason, guest_context);
		}
	}
	//---------------------------------------------------------------------------------------------------------------------//

	// Triple fault VM-exit. Fatal error.
	_Use_decl_annotations_ static void VmmpHandleTripleFault(
		GuestContext *guest_context) {
		VmmpDumpGuestSelectors();
		HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kTripleFaultVmExit,
			reinterpret_cast<ULONG_PTR>(guest_context),
			guest_context->ip, 0);
	}

	// Unexpected VM-exit. Fatal error.
	_Use_decl_annotations_ static void VmmpHandleUnexpectedExit(
		GuestContext *guest_context) {
		VmmpDumpGuestSelectors();

		HYPERPLATFORM_LOG_DEBUG_SAFE("%I64X %I64X %I64X", UtilVmRead(VmcsField::kGuestRip), UtilVmRead(VmcsField::kVmExitReason), UtilVmRead(VmcsField::kVmInstructionError));

		HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnexpectedVmExit,
			reinterpret_cast<ULONG_PTR>(guest_context), UtilVmRead(VmcsField::kVmExitReason),
			UtilVmRead(VmcsField::kVmInstructionError));
	}

	// MTF VM-exit
	_Use_decl_annotations_ static void VmmpHandleMonitorTrap(GuestContext *guest_context)
	{
		UNREFERENCED_PARAMETER(guest_context);
	}


	// Interrupt
	_Use_decl_annotations_ static void VmmpHandleException(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const VmExitInterruptionInformationField exception = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo)) };

		const auto interruption_type =
			static_cast<InterruptionType>(exception.fields.interruption_type);

		const auto vector = static_cast<InterruptionVector>(exception.fields.vector);

		if (interruption_type == InterruptionType::kHardwareException) {
			// Hardware exception
			if (vector == InterruptionVector::kPageFaultException) {

				// #PF
				const PageFaultErrorCode fault_code = {
					static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrErrorCode)) };
				const auto fault_address = UtilVmRead(VmcsField::kExitQualification);

				VmmpInjectInterruption(interruption_type, vector, exception.fields.error_code_valid, fault_code.all);

				AsmWriteCR2(fault_address);

			}
			else if (vector == InterruptionVector::kGeneralProtectionException) {
				// # GP
				const auto error_code =
					static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrErrorCode));

				VmmpInjectInterruption(interruption_type, vector, true, error_code);
			}

			else {

				HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
					0);
			}

		}
		else if (interruption_type == InterruptionType::kSoftwareException) {
			// Software exception
			if (vector == InterruptionVector::kBreakpointException) {
				// #BP
				VmmpInjectInterruption(interruption_type, vector, false, 0);
				//HYPERPLATFORM_LOG_INFO_SAFE("L0 GuestIp= %p, #BP ", guest_context->ip);
				UtilVmWrite(VmcsField::kVmEntryInstructionLen, 1);

			}
			else if (vector == InterruptionVector::kApcInterrupt)
			{
				const auto error_code =
					static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrErrorCode));
				HYPERPLATFORM_LOG_DEBUG("RIP: %i64x RSP: %i64x \r\n", UtilVmRead(VmcsField::kGuestRip), UtilVmRead(VmcsField::kGuestRsp));
				VmmpInjectInterruption(interruption_type, vector, exception.fields.error_code_valid, error_code);
			}

			else {
				HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
					0);
			}
		}
		else {
			HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
				0);
		}
	}
	//  L2 -> L0	cpuid 
	//  L0 -> L1	inject cpuid
	//  L1 -> L0	vmresume
	//  L0 -> L2	resume l2 

	// CPUID
	_Use_decl_annotations_ static void VmmpHandleCpuid(
		_In_ GuestContext *guest_context
	)
	{
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		unsigned int cpu_info[4] = {};
		const auto function_id = static_cast<int>(guest_context->gp_regs->ax);
		const auto sub_function_id = static_cast<int>(guest_context->gp_regs->cx);

		__cpuidex(reinterpret_cast<int *>(cpu_info), function_id, sub_function_id);

		if (function_id == 1)
		{
			// Present existence of a hypervisor using the HypervisorPresent bit
			CpuFeaturesEcx cpu_features = { static_cast<ULONG_PTR>(cpu_info[2]) };
			cpu_features.fields.not_used = false;
			cpu_info[2] = static_cast<int>(cpu_features.all);
		}
		else if (function_id == kHyperVCpuidInterface)
		{
			// Leave signature of HyperPlatform onto EAX
			cpu_info[0] = 'PpyH';
		}

		guest_context->gp_regs->ax = cpu_info[0];
		guest_context->gp_regs->bx = cpu_info[1];
		guest_context->gp_regs->cx = cpu_info[2];
		guest_context->gp_regs->dx = cpu_info[3];

		HYPERPLATFORM_LOG_DEBUG_SAFE("Root CPUID Called with id : %x sid: %x !!!!!!!!!!!!!!! \r\n", function_id, sub_function_id);
		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// RDTSC
	_Use_decl_annotations_ static void VmmpHandleRdtsc(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		ULARGE_INTEGER tsc = {};
		tsc.QuadPart = __rdtsc();
		guest_context->gp_regs->dx = tsc.HighPart;
		guest_context->gp_regs->ax = tsc.LowPart;

		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// RDTSCP
	_Use_decl_annotations_ static void VmmpHandleRdtscp(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		unsigned int tsc_aux = 0;
		ULARGE_INTEGER tsc = {};
		tsc.QuadPart = __rdtscp(&tsc_aux);
		guest_context->gp_regs->dx = tsc.HighPart;
		guest_context->gp_regs->ax = tsc.LowPart;
		guest_context->gp_regs->cx = tsc_aux;

		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// XSETBV. It is executed at the time of system resuming
	_Use_decl_annotations_ static void VmmpHandleXsetbv(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		ULARGE_INTEGER value = {};
		value.LowPart = static_cast<ULONG>(guest_context->gp_regs->ax);
		value.HighPart = static_cast<ULONG>(guest_context->gp_regs->dx);
		_xsetbv(static_cast<ULONG>(guest_context->gp_regs->cx), value.QuadPart);

		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// RDMSR
	_Use_decl_annotations_ static void VmmpHandleMsrReadAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		VmmpHandleMsrAccess(guest_context, true);
	}

	// WRMSR
	_Use_decl_annotations_ static void VmmpHandleMsrWriteAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		VmmpHandleMsrAccess(guest_context, false);
	}

	// RDMSR and WRMSR
	_Use_decl_annotations_ static void VmmpHandleMsrAccess(
		GuestContext *guest_context, bool read_access) {
		// Apply it for VMCS instead of a real MSR if a specified MSR is either of
		// them.
		const auto msr = static_cast<Msr>(guest_context->gp_regs->cx);

		bool transfer_to_vmcs = false;
		VmcsField vmcs_field = {};
		switch (msr) {
		case Msr::kIa32SysenterCs:
			vmcs_field = VmcsField::kGuestSysenterCs;
			transfer_to_vmcs = true;
			break;
		case Msr::kIa32SysenterEsp:
			vmcs_field = VmcsField::kGuestSysenterEsp;
			transfer_to_vmcs = true;
			break;
		case Msr::kIa32SysenterEip:
			vmcs_field = VmcsField::kGuestSysenterEip;
			transfer_to_vmcs = true;
			break;
		case Msr::kIa32Debugctl:
			vmcs_field = VmcsField::kGuestIa32Debugctl;
			transfer_to_vmcs = true;
			break;
		case Msr::kIa32GsBase:
		{
			vmcs_field = VmcsField::kGuestGsBase;
			transfer_to_vmcs = true;
			break;

		}
		case Msr::kIa32KernelGsBase:
		{
			transfer_to_vmcs = false;
			break;
		}
		case Msr::kIa32FsBase:
			vmcs_field = VmcsField::kGuestFsBase;
			transfer_to_vmcs = true;
			break;
		default:
			break;
		}

		const auto is_64bit_vmcs =
			UtilIsInBounds(vmcs_field, VmcsField::kIoBitmapA,
				VmcsField::kHostIa32PerfGlobalCtrlHigh);

		LARGE_INTEGER msr_value = {};
		if (read_access)
		{
			if (transfer_to_vmcs)
			{
				if (is_64bit_vmcs)
				{
					msr_value.QuadPart = UtilVmRead64(vmcs_field);
				}
				else
				{
					msr_value.QuadPart = UtilVmRead(vmcs_field);
				}
			}
			else
			{
				if (msr == Msr::kIa32VmxEptVpidCap)
				{
					msr_value.LowPart = guest_context->stack->processor_data->VmxEptMsr.LowPart;
					msr_value.HighPart = guest_context->stack->processor_data->VmxEptMsr.HighPart;
				}
				else if (msr == Msr::kIa32FeatureControl)
				{
					msr_value.LowPart = guest_context->stack->processor_data->Ia32FeatureMsr.LowPart;
					msr_value.HighPart = guest_context->stack->processor_data->Ia32FeatureMsr.HighPart;
					HYPERPLATFORM_LOG_DEBUG("Writing Ia32FeactureCtrl : %I64x \r\n ", msr_value.QuadPart);
				}
				else if (msr == Msr::kIa32VmxBasic)
				{
					msr_value.LowPart = guest_context->stack->processor_data->VmxBasicMsr.LowPart;
					msr_value.HighPart = guest_context->stack->processor_data->VmxBasicMsr.HighPart;
				}
				else
				{
					msr_value.QuadPart = UtilReadMsr64(msr);
				}
			}
			guest_context->gp_regs->ax = msr_value.LowPart;
			guest_context->gp_regs->dx = msr_value.HighPart;
		}
		else
		{
			msr_value.LowPart = static_cast<ULONG>(guest_context->gp_regs->ax);
			msr_value.HighPart = static_cast<ULONG>(guest_context->gp_regs->dx);
			if (transfer_to_vmcs)
			{
				if (is_64bit_vmcs)
				{
					UtilVmWrite64(vmcs_field, static_cast<ULONG_PTR>(msr_value.QuadPart));
				}
				else
				{
					UtilVmWrite(vmcs_field, static_cast<ULONG_PTR>(msr_value.QuadPart));
				}
			}
			else
			{
				if (msr == Msr::kIa32VmxEptVpidCap)
				{
					guest_context->stack->processor_data->VmxEptMsr.QuadPart = msr_value.QuadPart;
				}
				else if (msr == Msr::kIa32FeatureControl)
				{
					guest_context->stack->processor_data->Ia32FeatureMsr.QuadPart = msr_value.QuadPart;
					HYPERPLATFORM_LOG_DEBUG("Reading Ia32FeactureCtrl : %I64x \r\n ", msr_value.QuadPart);
				}
				else if (msr == Msr::kIa32VmxBasic)
				{
					guest_context->stack->processor_data->VmxBasicMsr.QuadPart = msr_value.QuadPart;
				}
				else
				{
					UtilWriteMsr64(msr, msr_value.QuadPart);
				}
			}
		}

		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// LIDT, SIDT, LGDT and SGDT
	_Use_decl_annotations_ static void VmmpHandleGdtrOrIdtrAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const GdtrOrIdtrInstInformation exit_qualification = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmxInstructionInfo)) };

		// Calculate an address to be used for the instruction
		const auto displacement = UtilVmRead(VmcsField::kExitQualification);

		// Base
		ULONG_PTR base_value = 0;
		if (!exit_qualification.fields.base_register_invalid) {
			const auto register_used = VmmpSelectRegister(
				exit_qualification.fields.base_register, guest_context);
			base_value = *register_used;
		}

		// Index
		ULONG_PTR index_value = 0;
		if (!exit_qualification.fields.index_register_invalid) {
			const auto register_used = VmmpSelectRegister(
				exit_qualification.fields.index_register, guest_context);
			index_value = *register_used;
			switch (static_cast<Scaling>(exit_qualification.fields.scalling)) {
			case Scaling::kNoScaling:
				index_value = index_value;
				break;
			case Scaling::kScaleBy2:
				index_value = index_value * 2;
				break;
			case Scaling::kScaleBy4:
				index_value = index_value * 4;
				break;
			case Scaling::kScaleBy8:
				index_value = index_value * 8;
				break;
			default:
				break;
			}
		}

		auto operation_address = base_value + index_value + displacement;
		if (static_cast<AddressSize>(exit_qualification.fields.address_size) ==
			AddressSize::k32bit) {
			operation_address &= MAXULONG;
		}

		// Update CR3 with that of the guest since below code is going to access
		// memory.
		const auto guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
		const auto vmm_cr3 = __readcr3();
		__writecr3(guest_cr3);

		// Emulate the instruction
		auto descriptor_table_reg = reinterpret_cast<Idtr *>(operation_address);
		switch (static_cast<GdtrOrIdtrInstructionIdentity>(
			exit_qualification.fields.instruction_identity)) {
		case GdtrOrIdtrInstructionIdentity::kSgdt:
			descriptor_table_reg->base = UtilVmRead(VmcsField::kGuestGdtrBase);
			descriptor_table_reg->limit =
				static_cast<unsigned short>(UtilVmRead(VmcsField::kGuestGdtrLimit));
			break;
		case GdtrOrIdtrInstructionIdentity::kSidt:
			descriptor_table_reg->base = UtilVmRead(VmcsField::kGuestIdtrBase);
			descriptor_table_reg->limit =
				static_cast<unsigned short>(UtilVmRead(VmcsField::kGuestIdtrLimit));
			break;
		case GdtrOrIdtrInstructionIdentity::kLgdt:
			UtilVmWrite(VmcsField::kGuestGdtrBase, descriptor_table_reg->base);
			UtilVmWrite(VmcsField::kGuestGdtrLimit, descriptor_table_reg->limit);
			break;
		case GdtrOrIdtrInstructionIdentity::kLidt:
			UtilVmWrite(VmcsField::kGuestIdtrBase, descriptor_table_reg->base);
			UtilVmWrite(VmcsField::kGuestIdtrLimit, descriptor_table_reg->limit);
			break;
		}

		__writecr3(vmm_cr3);
		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// LLDT, LTR, SLDT, and STR
	_Use_decl_annotations_ static void VmmpHandleLdtrOrTrAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const LdtrOrTrInstInformation exit_qualification = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmxInstructionInfo)) };

		// Calculate an address or a register to be used for the instruction
		const auto displacement = UtilVmRead(VmcsField::kExitQualification);

		ULONG_PTR operation_address = 0;
		if (exit_qualification.fields.register_access) {
			// Register
			const auto register_used =
				VmmpSelectRegister(exit_qualification.fields.register1, guest_context);
			operation_address = reinterpret_cast<ULONG_PTR>(register_used);
		}
		else {
			// Base
			ULONG_PTR base_value = 0;
			if (!exit_qualification.fields.base_register_invalid) {
				const auto register_used = VmmpSelectRegister(
					exit_qualification.fields.base_register, guest_context);
				base_value = *register_used;
			}

			// Index
			ULONG_PTR index_value = 0;
			if (!exit_qualification.fields.index_register_invalid) {
				const auto register_used = VmmpSelectRegister(
					exit_qualification.fields.index_register, guest_context);
				index_value = *register_used;
				switch (static_cast<Scaling>(exit_qualification.fields.scalling)) {
				case Scaling::kNoScaling:
					index_value = index_value;
					break;
				case Scaling::kScaleBy2:
					index_value = index_value * 2;
					break;
				case Scaling::kScaleBy4:
					index_value = index_value * 4;
					break;
				case Scaling::kScaleBy8:
					index_value = index_value * 8;
					break;
				default:
					break;
				}
			}

			operation_address = base_value + index_value + displacement;
			if (static_cast<AddressSize>(exit_qualification.fields.address_size) ==
				AddressSize::k32bit) {
				operation_address &= MAXULONG;
			}
		}

		// Update CR3 with that of the guest since below code is going to access
		// memory.
		const auto guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
		const auto vmm_cr3 = __readcr3();
		__writecr3(guest_cr3);

		// Emulate the instruction
		auto selector = reinterpret_cast<USHORT *>(operation_address);
		switch (static_cast<LdtrOrTrInstructionIdentity>(
			exit_qualification.fields.instruction_identity)) {
		case LdtrOrTrInstructionIdentity::kSldt:
			*selector = static_cast<USHORT>(UtilVmRead(VmcsField::kGuestLdtrSelector));
			break;
		case LdtrOrTrInstructionIdentity::kStr:
			*selector = static_cast<USHORT>(UtilVmRead(VmcsField::kGuestTrSelector));
			break;
		case LdtrOrTrInstructionIdentity::kLldt:
			UtilVmWrite(VmcsField::kGuestLdtrSelector, *selector);
			break;
		case LdtrOrTrInstructionIdentity::kLtr:
			UtilVmWrite(VmcsField::kGuestTrSelector, *selector);
			break;
		default:
			break;
		}

		__writecr3(vmm_cr3);
		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// MOV to / from DRx
	_Use_decl_annotations_ static void VmmpHandleDrAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const MovDrQualification exit_qualification = {
			UtilVmRead(VmcsField::kExitQualification) };
		const auto register_used =
			VmmpSelectRegister(exit_qualification.fields.gp_register, guest_context);

		// Emulate the instruction
		switch (static_cast<MovDrDirection>(exit_qualification.fields.direction)) {
		case MovDrDirection::kMoveToDr:
			// clang-format off
			switch (exit_qualification.fields.debugl_register) {
			case 0: __writedr(0, *register_used); break;
			case 1: __writedr(1, *register_used); break;
			case 2: __writedr(2, *register_used); break;
			case 3: __writedr(3, *register_used); break;
			case 4: __writedr(4, *register_used); break;
			case 5: __writedr(5, *register_used); break;
			case 6: __writedr(6, *register_used); break;
			case 7: UtilVmWrite(VmcsField::kGuestDr7, *register_used); break;
			default: break;
			}
			// clang-format on
			break;
		case MovDrDirection::kMoveFromDr:
			// clang-format off
			switch (exit_qualification.fields.debugl_register) {
			case 0: *register_used = __readdr(0); break;
			case 1: *register_used = __readdr(1); break;
			case 2: *register_used = __readdr(2); break;
			case 3: *register_used = __readdr(3); break;
			case 4: *register_used = __readdr(4); break;
			case 5: *register_used = __readdr(5); break;
			case 6: *register_used = __readdr(6); break;
			case 7: *register_used = UtilVmRead(VmcsField::kGuestDr7); break;
			default: break;
			}
			// clang-format on
			break;
		default:
			HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
				0);
			break;
		}

		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// IN, INS, OUT, OUTS
	_Use_decl_annotations_ static void VmmpHandleIoPort(
		GuestContext *guest_context) {
		const IoInstQualification exit_qualification = {
			UtilVmRead(VmcsField::kExitQualification) };

		const auto is_in = exit_qualification.fields.direction == 1;  // to memory?
		const auto is_string = exit_qualification.fields.string_instruction == 1;
		const auto is_rep = exit_qualification.fields.rep_prefixed == 1;
		const auto port = static_cast<USHORT>(exit_qualification.fields.port_number);
		const auto string_address = reinterpret_cast<void *>(
			(is_in) ? guest_context->gp_regs->di : guest_context->gp_regs->si);
		const auto count =
			static_cast<unsigned long>((is_rep) ? guest_context->gp_regs->cx : 1);
		const auto address =
			(is_string) ? string_address : &guest_context->gp_regs->ax;

		SIZE_T size_of_access = 0;
		const char *suffix = "";
		switch (static_cast<IoInstSizeOfAccess>(
			exit_qualification.fields.size_of_access)) {
		case IoInstSizeOfAccess::k1Byte:
			size_of_access = 1;
			suffix = "B";
			break;
		case IoInstSizeOfAccess::k2Byte:
			size_of_access = 2;
			suffix = "W";
			break;
		case IoInstSizeOfAccess::k4Byte:
			size_of_access = 4;
			suffix = "D";
			break;
		}

		HYPERPLATFORM_LOG_DEBUG_SAFE("GuestIp= %p, Port= %04x, %s%s%s",
			guest_context->ip, port, (is_in ? "IN" : "OUT"),
			(is_string ? "S" : ""),
			(is_string ? suffix : ""));

		VmmpIoWrapper(is_in, is_string, size_of_access, port, address, count);

		// Update RCX, RDI and RSI accordingly. Note that this code can handle only
		// the REP prefix.
		if (is_string) {
			const auto update_count = (is_rep) ? guest_context->gp_regs->cx : 1;
			const auto update_size = update_count * size_of_access;
			const auto update_register =
				(is_in) ? &guest_context->gp_regs->di : &guest_context->gp_regs->si;

			if (guest_context->flag_reg.fields.df) {
				*update_register = *update_register - update_size;
			}
			else {
				*update_register = *update_register + update_size;
			}

			if (is_rep) {
				guest_context->gp_regs->cx = 0;
			}
		}

		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// Perform IO instruction according with parameters
	_Use_decl_annotations_ static void VmmpIoWrapper(bool to_memory, bool is_string,
		SIZE_T size_of_access,
		unsigned short port,
		void *address,
		unsigned long count) {
		NT_ASSERT(size_of_access == 1 || size_of_access == 2 || size_of_access == 4);

		// Update CR3 with that of the guest since below code is going to access
		// memory.
		const auto guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
		const auto vmm_cr3 = __readcr3();
		__writecr3(guest_cr3);

		// clang-format off
		if (to_memory) {
			if (is_string) {
				// INS
				switch (size_of_access) {
				case 1: __inbytestring(port, reinterpret_cast<UCHAR*>(address), count); break;
				case 2: __inwordstring(port, reinterpret_cast<USHORT*>(address), count); break;
				case 4: __indwordstring(port, reinterpret_cast<ULONG*>(address), count); break;
				}
			}
			else {
				// IN
				switch (size_of_access) {
				case 1: *reinterpret_cast<UCHAR*>(address) = __inbyte(port); break;
				case 2: *reinterpret_cast<USHORT*>(address) = __inword(port); break;
				case 4: *reinterpret_cast<ULONG*>(address) = __indword(port); break;
				}
			}
		}
		else {
			if (is_string) {
				// OUTS
				switch (size_of_access) {
				case 1: __outbytestring(port, reinterpret_cast<UCHAR*>(address), count); break;
				case 2: __outwordstring(port, reinterpret_cast<USHORT*>(address), count); break;
				case 4: __outdwordstring(port, reinterpret_cast<ULONG*>(address), count); break;
				}
			}
			else {
				// OUT
				switch (size_of_access) {
				case 1: __outbyte(port, *reinterpret_cast<UCHAR*>(address)); break;
				case 2: __outword(port, *reinterpret_cast<USHORT*>(address)); break;
				case 4: __outdword(port, *reinterpret_cast<ULONG*>(address)); break;
				}
			}
		}
		// clang-format on

		__writecr3(vmm_cr3);
	}

	// MOV to / from CRx
	_Use_decl_annotations_ static void VmmpHandleCrAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const MovCrQualification exit_qualification = {
			UtilVmRead(VmcsField::kExitQualification) };

		const auto register_used =
			VmmpSelectRegister(exit_qualification.fields.gp_register, guest_context);

		switch (static_cast<MovCrAccessType>(exit_qualification.fields.access_type)) {
		case MovCrAccessType::kMoveToCr:
			switch (exit_qualification.fields.control_register) {
				// CR0 <- Reg
			case 0: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				if (UtilIsX86Pae()) {
					UtilLoadPdptes(UtilVmRead(VmcsField::kGuestCr3));
				}
				const Cr0 cr0_fixed0 = { UtilReadMsr(Msr::kIa32VmxCr0Fixed0) };
				const Cr0 cr0_fixed1 = { UtilReadMsr(Msr::kIa32VmxCr0Fixed1) };
				Cr0 cr0 = { *register_used };
				cr0.all &= cr0_fixed1.all;
				cr0.all |= cr0_fixed0.all;
				UtilVmWrite(VmcsField::kGuestCr0, cr0.all);
				UtilVmWrite(VmcsField::kCr0ReadShadow, cr0.all);
				break;
			}

					// CR3 <- Reg
			case 3: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				if (UtilIsX86Pae()) {
					UtilLoadPdptes(*register_used);
				}
				UtilInvvpidSingleContextExceptGlobal(
					static_cast<USHORT>(KeGetCurrentProcessorNumberEx(nullptr) + 1));

				UtilVmWrite(VmcsField::kGuestCr3, *register_used);
				break;
			}

					// CR4 <- Reg
			case 4: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				if (UtilIsX86Pae()) {
					UtilLoadPdptes(UtilVmRead(VmcsField::kGuestCr3));
				}
				UtilInvvpidAllContext();
				const Cr4 cr4_fixed0 = { UtilReadMsr(Msr::kIa32VmxCr4Fixed0) };
				const Cr4 cr4_fixed1 = { UtilReadMsr(Msr::kIa32VmxCr4Fixed1) };
				Cr4 cr4 = { *register_used };
				cr4.all &= cr4_fixed1.all;
				cr4.all |= cr4_fixed0.all;
				UtilVmWrite(VmcsField::kGuestCr4, cr4.all);
				UtilVmWrite(VmcsField::kCr4ReadShadow, cr4.all);
				break;
			}

					// CR8 <- Reg
			case 8: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				guest_context->cr8 = *register_used;
				break;
			}

			default:
				HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0,
					0, 0);
				break;
			}
			break;

		case MovCrAccessType::kMoveFromCr:
			switch (exit_qualification.fields.control_register) {
				// Reg <- CR3
			case 3: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				*register_used = UtilVmRead(VmcsField::kGuestCr3);
				break;
			}

					// Reg <- CR8
			case 8: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				*register_used = guest_context->cr8;
				break;
			}

			default:
				HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0,
					0, 0);
				break;
			}
			break;

			// Unimplemented
		case MovCrAccessType::kClts:
		case MovCrAccessType::kLmsw:
		default:
			HYPERPLATFORM_COMMON_DBG_BREAK();
			break;
		}

		VmmpAdjustGuestInstructionPointer(guest_context);
	}
	/*
	// VMX instructions except for VMCALL
	_Use_decl_annotations_ static void VmmpHandleVmx(GuestContext *guest_context) {
	HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
	// See "CONVENTIONS"
	guest_context->flag_reg.fields.cf = true;  // Error without status
	guest_context->flag_reg.fields.pf = false;
	guest_context->flag_reg.fields.af = false;
	guest_context->flag_reg.fields.zf = false;  // Error without status
	guest_context->flag_reg.fields.sf = false;
	guest_context->flag_reg.fields.of = false;
	UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all);
	VmmpAdjustGuestInstructionPointer(guest_context);
	}
	*/

	// VMCALL
	_Use_decl_annotations_ static void VmmpHandleVmCall(
		GuestContext *guest_context) {
		// VMCALL convention for HyperPlatform:
		//  ecx: hyper-call number (always 32bit)
		//  edx: arbitrary context parameter (pointer size)
		// Any unsuccessful VMCALL will inject #UD into a guest
		const auto hypercall_number =
			static_cast<HypercallNumber>(guest_context->gp_regs->cx);
		const auto context = reinterpret_cast<void *>(guest_context->gp_regs->dx);

		switch (hypercall_number) {
		case HypercallNumber::kTerminateVmm:
			// Unloading requested. This VMCALL is allowed to execute only from CPL=0
			if (VmmpGetGuestCpl() == 0) {
				VmmpHandleVmCallTermination(guest_context, context);
			}
			else {
				VmmpIndicateUnsuccessfulVmcall(guest_context);
			}
			break;
		case HypercallNumber::kPingVmm:
			// Sample VMCALL handler
			HYPERPLATFORM_LOG_INFO_SAFE("Pong by VMM! (context = %p)", context);
			VmmpIndicateSuccessfulVmcall(guest_context);
			break;
		case HypercallNumber::kGetSharedProcessorData:
			*reinterpret_cast<void **>(context) =
				guest_context->stack->processor_data->shared_data;
			VmmpIndicateSuccessfulVmcall(guest_context);
			break;
		default:
			// Unsupported hypercall
			VmmpIndicateUnsuccessfulVmcall(guest_context);
		}
	}

	// INVD
	_Use_decl_annotations_ static void VmmpHandleInvalidateInternalCaches(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		AsmInvalidateInternalCaches();
		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// INVLPG
	_Use_decl_annotations_ static void VmmpHandleInvalidateTlbEntry(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const auto invalidate_address =
			reinterpret_cast<void *>(UtilVmRead(VmcsField::kExitQualification));
		__invlpg(invalidate_address);
		UtilInvvpidIndividualAddress(
			static_cast<USHORT>(KeGetCurrentProcessorNumberEx(nullptr) + 1),
			invalidate_address);
		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// EXIT_REASON_EPT_VIOLATION
	_Use_decl_annotations_ static void VmmpHandleEptViolation(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		auto processor_data = guest_context->stack->processor_data;
		EptHandleEptViolation(processor_data->ept_data);
	}

	// EXIT_REASON_EPT_MISCONFIG
	_Use_decl_annotations_ static void VmmpHandleEptMisconfig(
		GuestContext *guest_context) {
		UNREFERENCED_PARAMETER(guest_context);

		const auto fault_address = UtilVmRead(VmcsField::kGuestPhysicalAddress);
		const auto ept_pt_entry = EptGetEptPtEntry(
			guest_context->stack->processor_data->ept_data, fault_address);
		HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kEptMisconfigVmExit,
			fault_address,
			reinterpret_cast<ULONG_PTR>(ept_pt_entry), 0);
	}

	// Selects a register to be used based on the index
	_Use_decl_annotations_ ULONG_PTR *VmmpSelectRegister(
		ULONG index, GuestContext *guest_context) {
		ULONG_PTR *register_used = nullptr;
		// clang-format off
		switch (index) {
		case 0: register_used = &guest_context->gp_regs->ax; break;
		case 1: register_used = &guest_context->gp_regs->cx; break;
		case 2: register_used = &guest_context->gp_regs->dx; break;
		case 3: register_used = &guest_context->gp_regs->bx; break;
		case 4: register_used = &guest_context->gp_regs->sp; break;
		case 5: register_used = &guest_context->gp_regs->bp; break;
		case 6: register_used = &guest_context->gp_regs->si; break;
		case 7: register_used = &guest_context->gp_regs->di; break;
#if defined(_AMD64_)
		case 8: register_used = &guest_context->gp_regs->r8; break;
		case 9: register_used = &guest_context->gp_regs->r9; break;
		case 10: register_used = &guest_context->gp_regs->r10; break;
		case 11: register_used = &guest_context->gp_regs->r11; break;
		case 12: register_used = &guest_context->gp_regs->r12; break;
		case 13: register_used = &guest_context->gp_regs->r13; break;
		case 14: register_used = &guest_context->gp_regs->r14; break;
		case 15: register_used = &guest_context->gp_regs->r15; break;
#endif
		default: HYPERPLATFORM_COMMON_DBG_BREAK(); break;
		}
		// clang-format on
		return register_used;
	}

	// Dumps guest's selectors
	/*_Use_decl_annotations_*/ static void VmmpDumpGuestSelectors() {
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"es %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestEsSelector),
			UtilVmRead(VmcsField::kGuestEsBase), UtilVmRead(VmcsField::kGuestEsLimit),
			UtilVmRead(VmcsField::kGuestEsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"cs %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestCsSelector),
			UtilVmRead(VmcsField::kGuestCsBase), UtilVmRead(VmcsField::kGuestCsLimit),
			UtilVmRead(VmcsField::kGuestCsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"ss %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestSsSelector),
			UtilVmRead(VmcsField::kGuestSsBase), UtilVmRead(VmcsField::kGuestSsLimit),
			UtilVmRead(VmcsField::kGuestSsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"ds %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestDsSelector),
			UtilVmRead(VmcsField::kGuestDsBase), UtilVmRead(VmcsField::kGuestDsLimit),
			UtilVmRead(VmcsField::kGuestDsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"fs %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestFsSelector),
			UtilVmRead(VmcsField::kGuestFsBase), UtilVmRead(VmcsField::kGuestFsLimit),
			UtilVmRead(VmcsField::kGuestFsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"gs %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestGsSelector),
			UtilVmRead(VmcsField::kGuestGsBase), UtilVmRead(VmcsField::kGuestGsLimit),
			UtilVmRead(VmcsField::kGuestGsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE("ld %04x %p %08x %08x",
			UtilVmRead(VmcsField::kGuestLdtrSelector),
			UtilVmRead(VmcsField::kGuestLdtrBase),
			UtilVmRead(VmcsField::kGuestLdtrLimit),
			UtilVmRead(VmcsField::kGuestLdtrArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"tr %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestTrSelector),
			UtilVmRead(VmcsField::kGuestTrBase), UtilVmRead(VmcsField::kGuestTrLimit),
			UtilVmRead(VmcsField::kGuestTrArBytes));
	}

	// Advances guest's IP to the next instruction
	_Use_decl_annotations_ static void VmmpAdjustGuestInstructionPointer(
		GuestContext *guest_context) {
		const auto exit_inst_length = UtilVmRead(VmcsField::kVmExitInstructionLen);
		UtilVmWrite(VmcsField::kGuestRip, guest_context->ip + exit_inst_length);

		// Inject #DB if TF is set
		if (guest_context->flag_reg.fields.tf) {
			VmmpInjectInterruption(InterruptionType::kHardwareException,
				InterruptionVector::kDebugException, false, 0);
			UtilVmWrite(VmcsField::kVmEntryInstructionLen, exit_inst_length);
		}
	}

	// Handle VMRESUME or VMXOFF failure. Fatal error.
	_Use_decl_annotations_ void __stdcall VmmVmxFailureHandler(
		AllRegisters *all_regs) {
		const auto guest_ip = UtilVmRead(VmcsField::kGuestRip);
		// See: VM-Instruction Error Numbers
		const auto vmx_error = (all_regs->flags.fields.zf)
			? UtilVmRead(VmcsField::kVmInstructionError)
			: 0;
		HYPERPLATFORM_COMMON_BUG_CHECK(
			HyperPlatformBugCheck::kCriticalVmxInstructionFailure, vmx_error, 0, 0);
	}

	// Saves all supported user state components (x87, SSE, AVX states)
	_Use_decl_annotations_ static void VmmpSaveExtendedProcessorState(
		GuestContext *guest_context) {
		// Clear the TS flag temporarily since XSAVE/XRSTOR raise #NM
		Cr0 cr0 = { __readcr0() };
		const auto old_cr0 = cr0;
		cr0.fields.ts = false;
		__writecr0(cr0.all);
		if (guest_context->stack->processor_data->xsave_inst_mask) {
			_xsave(guest_context->stack->processor_data->xsave_area,
				guest_context->stack->processor_data->xsave_inst_mask);
		}
		else {
			// Advances an address up to 15 bytes to be 16-byte aligned
			auto alignment = reinterpret_cast<ULONG_PTR>(
				guest_context->stack->processor_data->fxsave_area) %
				16;
			alignment = (alignment) ? 16 - alignment : 0;
			_fxsave(guest_context->stack->processor_data->fxsave_area + alignment);
		}
		__writecr0(old_cr0.all);


	}

	// Restores all supported user state components (x87, SSE, AVX states)
	_Use_decl_annotations_ static void VmmpRestoreExtendedProcessorState(
		GuestContext *guest_context) {
		// Clear the TS flag temporarily since XSAVE/XRSTOR raise #NM
		Cr0 cr0 = { __readcr0() };
		const auto old_cr0 = cr0;
		cr0.fields.ts = false;
		__writecr0(cr0.all);
		if (guest_context->stack->processor_data->xsave_inst_mask) {
			_xrstor(guest_context->stack->processor_data->xsave_area,
				guest_context->stack->processor_data->xsave_inst_mask);
		}
		else {
			// Advances an address up to 15 bytes to be 16-byte aligned
			auto alignment = reinterpret_cast<ULONG_PTR>(
				guest_context->stack->processor_data->fxsave_area) %
				16;
			alignment = (alignment) ? 16 - alignment : 0;
			_fxsave(guest_context->stack->processor_data->fxsave_area + alignment);
		}
		__writecr0(old_cr0.all);
	}

	// Indicates successful VMCALL
	_Use_decl_annotations_ static void VmmpIndicateSuccessfulVmcall(
		GuestContext *guest_context) {
		// See "CONVENTIONS"
		guest_context->flag_reg.fields.cf = false;
		guest_context->flag_reg.fields.pf = false;
		guest_context->flag_reg.fields.af = false;
		guest_context->flag_reg.fields.zf = false;
		guest_context->flag_reg.fields.sf = false;
		guest_context->flag_reg.fields.of = false;
		guest_context->flag_reg.fields.cf = false;
		guest_context->flag_reg.fields.zf = false;
		UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all);
		VmmpAdjustGuestInstructionPointer(guest_context);
	}

	// Indicates unsuccessful VMCALL
	_Use_decl_annotations_ static void VmmpIndicateUnsuccessfulVmcall(
		GuestContext *guest_context) {
		UNREFERENCED_PARAMETER(guest_context);

		VmmpInjectInterruption(InterruptionType::kHardwareException,
			InterruptionVector::kInvalidOpcodeException, false, 0);
		UtilVmWrite(VmcsField::kVmEntryInstructionLen, 3);  // VMCALL is 3 bytes
	}

	// Handles an unloading request
	_Use_decl_annotations_ static void VmmpHandleVmCallTermination(
		GuestContext *guest_context, void *context) {
		// The processor sets ffff to limits of IDT and GDT when VM-exit occurred.
		// It is not correct value but fine to ignore since vmresume loads correct
		// values from VMCS. But here, we are going to skip vmresume and simply
		// return to where VMCALL is executed. It results in keeping those broken
		// values and ends up with bug check 109, so we should fix them manually.
		const auto gdt_limit = UtilVmRead(VmcsField::kGuestGdtrLimit);
		const auto gdt_base = UtilVmRead(VmcsField::kGuestGdtrBase);
		const auto idt_limit = UtilVmRead(VmcsField::kGuestIdtrLimit);
		const auto idt_base = UtilVmRead(VmcsField::kGuestIdtrBase);
		Gdtr gdtr = { static_cast<USHORT>(gdt_limit), gdt_base };
		Idtr idtr = { static_cast<USHORT>(idt_limit), idt_base };
		__lgdt(&gdtr);
		__lidt(&idtr);

		// Store an address of the management structure to the context parameter
		const auto result_ptr = reinterpret_cast<ProcessorData **>(context);
		*result_ptr = guest_context->stack->processor_data;
		HYPERPLATFORM_LOG_DEBUG_SAFE("Context at %p %p", context,
			guest_context->stack->processor_data);

		// Set rip to the next instruction of VMCALL
		const auto exit_instruction_length =
			UtilVmRead(VmcsField::kVmExitInstructionLen);
		const auto return_address = guest_context->ip + exit_instruction_length;

		// Since the flag register is overwritten after VMXOFF, we should manually
		// indicates that VMCALL was successful by clearing those flags.
		// See "CONVENTIONS"
		guest_context->flag_reg.fields.cf = false;
		guest_context->flag_reg.fields.pf = false;
		guest_context->flag_reg.fields.af = false;
		guest_context->flag_reg.fields.zf = false;
		guest_context->flag_reg.fields.sf = false;
		guest_context->flag_reg.fields.of = false;
		guest_context->flag_reg.fields.cf = false;
		guest_context->flag_reg.fields.zf = false;

		// Set registers used after VMXOFF to recover the context. Volatile
		// registers must be used because those changes are reflected to the
		// guest's context after VMXOFF.
		guest_context->gp_regs->cx = return_address;
		guest_context->gp_regs->dx = guest_context->gp_regs->sp;
		guest_context->gp_regs->ax = guest_context->flag_reg.all;
		guest_context->vm_continue = false;
	}

	// Returns guest's CPL
	/*_Use_decl_annotations_*/ static UCHAR VmmpGetGuestCpl() {
		VmxRegmentDescriptorAccessRight ar = {
			static_cast<unsigned int>(UtilVmRead(VmcsField::kGuestSsArBytes)) };
		return ar.fields.dpl;
	}
	static bool IsBenignException(InterruptionVector vector)
	{
		switch (vector)
		{
		case InterruptionVector::kDebugException:
		case InterruptionVector::kNmiInterrupt:
		case InterruptionVector::kBreakpointException:
		case InterruptionVector::kOverflowException:
		case InterruptionVector::kBoundRangeExceededException:
		case InterruptionVector::kInvalidOpcodeException:
		case InterruptionVector::kDeviceNotAvailableException:
		case InterruptionVector::kCoprocessorSegmentOverrun:
		case InterruptionVector::kx87FpuFloatingPointError:
		case InterruptionVector::kAlignmentCheckException:
		case InterruptionVector::kMachineCheckException:
		case InterruptionVector::kSimdFloatingPointException:
			return true;
		default:
			return false;
		}
	}
	static bool IsContributoryException(InterruptionVector vector)
	{
		switch (vector)
		{
		case InterruptionVector::kDivideErrorException:
		case InterruptionVector::kInvalidTssException:
		case InterruptionVector::kSegmentNotPresent:
		case InterruptionVector::kStackFaultException:
			return true;
		default:
			return false;
		}
	}
	// Injects interruption to a guest
	_Use_decl_annotations_ static void VmmpInjectInterruption(
		InterruptionType interruption_type, InterruptionVector vector,
		bool deliver_error_code, ULONG32 error_code)
	{
		IdtVectoringInformationField idt_vectoring = { UtilVmRead(VmcsField::kIdtVectoringInfoField) };
		if (idt_vectoring.fields.valid)
		{
			if (InterruptionType::kHardwareException != static_cast<InterruptionType>(idt_vectoring.fields.interruption_type)
				|| IsBenignException(static_cast<InterruptionVector>(idt_vectoring.fields.vector))
				|| IsBenignException(vector)
				|| (IsContributoryException(static_cast<InterruptionVector>(idt_vectoring.fields.vector)) && InterruptionVector::kPageFaultException == vector)
				)
			{
				VmEntryInterruptionInformationField inject = {};
				inject.fields.valid = true;
				inject.fields.interruption_type = static_cast<ULONG32>(interruption_type);
				inject.fields.vector = static_cast<ULONG32>(vector);
				inject.fields.deliver_error_code = deliver_error_code;
				UtilVmWrite(VmcsField::kVmEntryIntrInfoField, inject.all);

				if (deliver_error_code) {
					UtilVmWrite(VmcsField::kVmEntryExceptionErrorCode, error_code);
				}

			}
			else if (InterruptionType::kHardwareException == static_cast<InterruptionType>(idt_vectoring.fields.interruption_type))
			{
				if ((IsContributoryException(vector) && IsContributoryException(static_cast<InterruptionVector>(idt_vectoring.fields.vector)))
					|| ((IsContributoryException(vector) || InterruptionVector::kPageFaultException == vector) &&
						static_cast<InterruptionVector>(idt_vectoring.fields.vector) == InterruptionVector::kPageFaultException
						)
					)
				{
					//double fault
					VmEntryInterruptionInformationField inject = {};
					inject.fields.valid = true;
					inject.fields.interruption_type = static_cast<ULONG32>(InterruptionType::kHardwareException);
					inject.fields.vector = static_cast<ULONG32>(InterruptionVector::kDoubleFaultException);
					inject.fields.deliver_error_code = 1;
					UtilVmWrite(VmcsField::kVmEntryIntrInfoField, inject.all);
					UtilVmWrite(VmcsField::kVmEntryExceptionErrorCode, 0);
				}
			}
			const PageFaultErrorCode fault_code = {
				static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrErrorCode)) };

			const auto fault_address = UtilVmRead(VmcsField::kExitQualification);

			PrintVMCS();

			HYPERPLATFORM_COMMON_DBG_BREAK();
		}
		else
		{
			VmEntryInterruptionInformationField inject = {};
			inject.fields.valid = true;
			inject.fields.interruption_type = static_cast<ULONG32>(interruption_type);
			inject.fields.vector = static_cast<ULONG32>(vector);
			inject.fields.deliver_error_code = deliver_error_code;
			UtilVmWrite(VmcsField::kVmEntryIntrInfoField, inject.all);

			if (deliver_error_code) {
				UtilVmWrite(VmcsField::kVmEntryExceptionErrorCode, error_code);
			}
		}
	}

	//----------------------------------------------------------------------------------------------------------------//
	_Use_decl_annotations_ static void VmmpHandleVmx(GuestContext *guest_context)
	{
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();


		//	Assumption:  If and only If trapped by L1 only,
		//    TODO	  :  Trapped by L2 handler

		const VmExitInformation exit_reason = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		switch (exit_reason.fields.reason)
		{
		case VmxExitReason::kVmon:
		{
			VmxonEmulate(guest_context);
		}
		break;

		/*
		Simpily clear VMCS02 and VMCS12
		*/
		case VmxExitReason::kVmclear:
		{
			VmclearEmulate(guest_context);
		}
		break;

		/*
		Load VMCS02 into physical cpu , And perform some check on VMCS12
		*/
		case VmxExitReason::kVmptrld:
		{
			VmptrldEmulate(guest_context);
		}
		break;

		case VmxExitReason::kVmptrst:
		{
			VmptrstEmulate(guest_context);
		}
		break;
		/// TODO:
		case VmxExitReason::kVmoff:
		{ 
			VmxoffEmulate(guest_context);
		}
		break;
		/*
		ReadWrite on VMCS12
		*/
		case VmxExitReason::kVmwrite:
		{
			VmwriteEmulate(guest_context);
		}
		break;
		/*
		ReadWrite on VMCS12
		*/
		case VmxExitReason::kVmread:
		{
			VmreadEmulate(guest_context);
		}
		break;

		case VmxExitReason::kVmlaunch:
		{
			VmlaunchEmulate(guest_context);
		}
		break;

		case VmxExitReason::kVmresume:
		{
			//In VmresumeEmulate will modify Guest Rip so no need to execute anymore after it returns.
			//And should not execute anymore.

			//Vmresume Emulation :
			//- Fill Guest, Host, Control field state in VMCS02
			//- Read GuestRIP from VMCS02 as it is trapped by L2 if Vmresume is trapped by L1
			//- So we need to help L1 to resume to L2
			//- We saved the vmcs02 GuestRip into VMCS12 our VMExit Handler, 
			//- because when L1 is executing VMRESUME, it is running on VMCS01
			VmresumeEmulate(guest_context);
		}
		break;

		case VmxExitReason::kVmxPreemptionTime:
		{
			VMSucceed(&guest_context->flag_reg);
			HYPERPLATFORM_LOG_DEBUG_SAFE("nested kVmwrite \r\n");
		}
		break;

		case VmxExitReason::kInvept:
		default:
		{
			VMSucceed(&guest_context->flag_reg);
			HYPERPLATFORM_LOG_DEBUG_SAFE("NOT SURE VMX \r\n");
		}
		break;
		}

		UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all);
		VmmpAdjustGuestInstructionPointer(guest_context);

	}
}  // extern "C"
