#include "MathUtils.h"
#include "Utilities.h"
#include "half.h"
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
using namespace RE;

const F4SE::TaskInterface* taskInterface;
PlayerCharacter* p;
bool ammoEventHooked = false;
bool reloaded = false;
bool shouldAddOne = false;
bool checkQueued = false;
uint32_t previousAmmoCount = 0;
uint16_t ammoCapacity = 0;
ActorValueInfo* BCRAVIF = nullptr;
BGSKeyword* chamberExclusion;
TESObjectWEAP::InstanceData* lastWeapon = nullptr;

void QueueCapacityCheck()
{
	if (checkQueued)
		return;
	checkQueued = true;

	std::thread([]() -> void {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (checkQueued) {
			taskInterface->AddTask([]() {
				if (*(bool*)((uintptr_t)p->currentProcess->high + 0x594)) {
					checkQueued = false;
					QueueCapacityCheck();
					return;
				}
				if (shouldAddOne && lastWeapon) {
					lastWeapon->ammoCapacity = ammoCapacity;
					shouldAddOne = false;
					lastWeapon = nullptr;
				}
				p->currentProcess->middleHigh->equippedItemsLock.lock();
				EquippedItem& equipped = p->currentProcess->middleHigh->equippedItems[0];
				TESObjectWEAP* wepForm = (TESObjectWEAP*)equipped.item.object;
				TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)equipped.item.instanceData.get();
				EquippedWeaponData* wepData = (EquippedWeaponData*)equipped.data.get();
				p->currentProcess->middleHigh->equippedItemsLock.unlock();
				if (wepData && wepForm->weaponData.skill != BCRAVIF && (!instance->keywords || !instance->keywords->HasKeyword(chamberExclusion, instance))) {
					previousAmmoCount = wepData->ammoCount;
					ammoCapacity = instance->ammoCapacity;
					instance->ammoCapacity = ammoCapacity + 1;
					shouldAddOne = true;
					lastWeapon = instance;
				}
				checkQueued = false;
			});
		}
	}).detach();
}

class AmmoEventWatcher : public BSTEventSink<PlayerAmmoCountEvent>
{
	virtual BSEventNotifyControl ProcessEvent(const PlayerAmmoCountEvent& evn, BSTEventSource<PlayerAmmoCountEvent>* src) override
	{
		if (shouldAddOne) {
			if (previousAmmoCount < evn.current && reloaded) {
				reloaded = false;
				p->currentProcess->middleHigh->equippedItemsLock.lock();
				EquippedItem& equipped = p->currentProcess->middleHigh->equippedItems[0];
				TESObjectWEAP* wepForm = (TESObjectWEAP*)equipped.item.object;
				TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)equipped.item.instanceData.get();
				EquippedWeaponData* wepData = (EquippedWeaponData*)equipped.data.get();
				p->currentProcess->middleHigh->equippedItemsLock.unlock();
				if (wepData && instance && instance->ammo) {
					uint32_t inventoryAmmoCount = 0;
					((TESObjectREFREx*)p)->GetItemCount(inventoryAmmoCount, instance->ammo, false);
					if (previousAmmoCount == 0) {
						wepData->ammoCount = min(ammoCapacity, inventoryAmmoCount);
					} else {
						wepData->ammoCount = min((uint32_t)(ammoCapacity + 1), inventoryAmmoCount);
					}
					taskInterface->AddUITask([]() {
						F4::GameUIModel* gameUIModel = *F4::ptr_GameUIModel;
						gameUIModel->UpdateDataModels();
					});
				}
			}
		}
		previousAmmoCount = evn.current;
		return BSEventNotifyControl::kContinue;
	}

public:
	static BSTEventSource<PlayerAmmoCountEvent>* GetEventSource()
	{
		for (auto it = (*F4::g_globalEvents.get())->eventSources.begin(); it != (*F4::g_globalEvents.get())->eventSources.end(); ++it) {
			const char* name = GetObjectClassName(*it) + 15;
			if (strstr(name, "PlayerAmmoCountEvent") == name) {
				return (BSTEventSource<PlayerAmmoCountEvent>*)&((*it)->src);
			}
		}
		return nullptr;
	}
	F4_HEAP_REDEFINE_NEW(AmmoEventWatcher);
};

class AnimationGraphEventWatcher
{
public:
	typedef BSEventNotifyControl (AnimationGraphEventWatcher::*FnProcessEvent)(BSAnimationGraphEvent& evn, BSTEventSource<BSAnimationGraphEvent>* dispatcher);

	BSEventNotifyControl HookedProcessEvent(BSAnimationGraphEvent& evn, BSTEventSource<BSAnimationGraphEvent>* src)
	{
		Actor* a = (Actor*)((uintptr_t)this - 0x38);
		if (a == p) {
			if (evn.animEvent == "reloadComplete" && shouldAddOne) {
				reloaded = true;
			} else if (evn.animEvent == "reloadEnd") {
				std::thread([]() -> void {
					std::this_thread::sleep_for(std::chrono::milliseconds(520));
					if (shouldAddOne && lastWeapon) {
						lastWeapon->ammoCapacity = ammoCapacity + 1;
					}
				}).detach();
			}
		}
		FnProcessEvent fn = fnHash.at(*(uintptr_t*)this);
		return fn ? (this->*fn)(evn, src) : BSEventNotifyControl::kContinue;
	}

	void HookSink()
	{
		uintptr_t vtable = *(uintptr_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnProcessEvent fn = SafeWrite64Function(vtable + 0x8, &AnimationGraphEventWatcher::HookedProcessEvent);
			fnHash.insert(std::pair<uintptr_t, FnProcessEvent>(vtable, fn));
		}
	}

protected:
	static std::unordered_map<uintptr_t, FnProcessEvent> fnHash;
};
std::unordered_map<uintptr_t, AnimationGraphEventWatcher::FnProcessEvent> AnimationGraphEventWatcher::fnHash;

class EquipWatcher : public BSTEventSink<TESEquipEvent>
{
public:
	virtual BSEventNotifyControl ProcessEvent(const TESEquipEvent& evn, BSTEventSource<TESEquipEvent>*)
	{
		if (evn.a == p) {
			TESForm* item = TESForm::GetFormByID(evn.formId);
			if (item && item->formType == ENUM_FORM_ID::kWEAP && ((TESObjectWEAP*)item)->weaponData.type == 9) {
				if (evn.flag != 0x00000000ff000000) {
					QueueCapacityCheck();
				}
			}
		}
		return BSEventNotifyControl::kContinue;
	}
	F4_HEAP_REDEFINE_NEW(EquipEventSink);
};

void ResetAmmoCountTracker()
{
	if (shouldAddOne && lastWeapon) {
		lastWeapon->ammoCapacity = ammoCapacity;
		lastWeapon = nullptr;
	}
	checkQueued = false;
	shouldAddOne = false;
	reloaded = false;
	if (p->currentProcess->middleHigh->equippedItems.size() == 0) {
		previousAmmoCount = 0;
		return;
	}

	p->currentProcess->middleHigh->equippedItemsLock.lock();
	EquippedItem& equipped = p->currentProcess->middleHigh->equippedItems[0];
	TESObjectWEAP* wepForm = (TESObjectWEAP*)equipped.item.object;
	TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)equipped.item.instanceData.get();
	EquippedWeaponData* wepData = (EquippedWeaponData*)equipped.data.get();
	p->currentProcess->middleHigh->equippedItemsLock.unlock();
	if (!wepForm || !instance || !wepData) {
		previousAmmoCount = 0;
		return;
	}

	if (instance->type != 9) {
		previousAmmoCount = 0;
		return;
	}

	previousAmmoCount = wepData->ammoCount;
	ammoCapacity = instance->ammoCapacity;
	if (wepData && wepForm->weaponData.skill != BCRAVIF && (!instance->keywords || !instance->keywords->HasKeyword(chamberExclusion, instance))) {
		instance->ammoCapacity = ammoCapacity + 1;
		lastWeapon = instance;
		shouldAddOne = true;
	}
}

void TryHookAmmoWatcher()
{
	if (ammoEventHooked)
		return;

	BSTEventSource<PlayerAmmoCountEvent>* ammoEventSource = AmmoEventWatcher::GetEventSource();
	if (ammoEventSource) {
		AmmoEventWatcher* aew = new AmmoEventWatcher();
		ammoEventSource->RegisterSink(aew);
		ammoEventHooked = true;
	}
}

void InitializePlugin()
{
	p = PlayerCharacter::GetSingleton();
	((AnimationGraphEventWatcher*)((uintptr_t)p + 0x38))->HookSink();
	EquipWatcher* ew = new EquipWatcher();
	EquipEventSource::GetSingleton()->RegisterSink(ew);
	BCRAVIF = (ActorValueInfo*)TESForm::GetFormByID(0x300);
	chamberExclusion = (BGSKeyword*)GetFormFromMod("UneducatedReload.esm", 0x800);
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::warn);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_f4se->IsEditor()) {
		logger::critical("loaded in editor"sv);
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		logger::critical(FMT_STRING("unsupported runtime v{}"), ver.string());
		return false;
	}

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	taskInterface = F4SE::GetTaskInterface();
	const F4SE::MessagingInterface* message = F4SE::GetMessagingInterface();
	message->RegisterListener([](F4SE::MessagingInterface::Message* msg) -> void {
		if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
			InitializePlugin();
		} else if (msg->type == F4SE::MessagingInterface::kPostLoadGame) {
			TryHookAmmoWatcher();
			ResetAmmoCountTracker();
		} else if (msg->type == F4SE::MessagingInterface::kNewGame) {
			TryHookAmmoWatcher();
			ResetAmmoCountTracker();
		}
	});

	return true;
}
