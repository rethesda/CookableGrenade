using namespace RE;

namespace
{
	PlayerCharacter* player = nullptr;
	PlayerControls* playerControls = nullptr;
	Setting* throwDelay = nullptr;
	float pinPullTime = 0.0F;
	float pinMaxTime = 0.0F;
	float cookTime = 0.0F;
	TESObjectWEAP* grenadeForm = nullptr;
	const F4SE::TaskInterface* taskInterface = nullptr;

	// The AE global is filled after cross-runtime data matching in Ghidra.
	REL::Relocation<float*> engineTime{ REL::ID{ 599343, 2712485 } };

	struct CookInputHook
	{
		using func_t = void(MeleeThrowHandler*, ButtonEvent*);
		static inline REL::Relocation<func_t> original;

		static void Thunk(MeleeThrowHandler* a_handler, ButtonEvent* a_event)
		{
			bool interruptThrow = false;
			if (a_event->value != 0.0F && a_event->heldDownSecs >= throwDelay->GetFloat()) {
				if (!a_handler->buttonHoldDebounce) {
					grenadeForm = nullptr;
					if (player->currentProcess && player->currentProcess->middleHigh) {
						auto* process = player->currentProcess->middleHigh;
						const RE::BSAutoLock lock(process->equippedItemsLock);
						for (auto& item : process->equippedItems) {
							if (item.equipIndex.index != 2) {
								continue;
							}
							auto* weapon = item.item.object ? item.item.object->As<TESObjectWEAP>() : nullptr;
							if (!weapon || !weapon->weaponData.rangedData) {
								continue;
							}
							auto* projectile = weapon->weaponData.rangedData->overrideProjectile;
							if (projectile && projectile->data.explosionProximity == 0.0F &&
								(projectile->data.flags & 0x4) != 0 && (projectile->data.flags & 0x20000) != 0) {
								grenadeForm = weapon;
								pinMaxTime = projectile->data.explosionTimer;
								break;
							}
						}
					}
					if (grenadeForm) {
						pinPullTime = *engineTime;
					}
				} else if (grenadeForm && *engineTime - pinPullTime >= pinMaxTime) {
					a_event->value = 0.0F;
					cookTime = pinMaxTime;
					BGSEquipIndex equipIndex{ 2 };
					TaskQueueInterface::GetSingleton()->QueueWeaponFire(
						grenadeForm, player, equipIndex, grenadeForm->weaponData.ammo);
					interruptThrow = true;
					a_handler->buttonHoldDebounce = false;
				}
			} else if (a_event->value == 0.0F && grenadeForm) {
				cookTime = *engineTime - pinPullTime;
			}

			original(a_handler, a_event);
			if (interruptThrow) {
				player->NotifyAnimationGraphImpl("weapForceEquip");
			}
		}

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtable{ MeleeThrowHandler::VTABLE[0] };
			original = vtable.write_vfunc(0x08, Thunk);
		}
	};

	struct GrenadeProjectileHook
	{
		using func_t = void(GrenadeProjectile*);
		static inline REL::Relocation<func_t> original;

		static void Thunk(GrenadeProjectile* a_projectile)
		{
			original(a_projectile);
			if (a_projectile->shooter.get().get() == player && a_projectile->weaponSource.object == grenadeForm) {
				taskInterface->AddTask([a_projectile]() {
					a_projectile->explosionTimer -= cookTime;
					grenadeForm = nullptr;
				});
			}
		}

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtable{ GrenadeProjectile::VTABLE[0] };
			original = vtable.write_vfunc(0xE8, Thunk);
		}
	};

	struct AnimationGraphHook
	{
		using func_t = BSEventNotifyControl(
			BSTEventSink<BSAnimationGraphEvent>*, const BSAnimationGraphEvent&,
			BSTEventSource<BSAnimationGraphEvent>*);
		static inline REL::Relocation<func_t> original;

		static BSEventNotifyControl Thunk(
			BSTEventSink<BSAnimationGraphEvent>* a_sink,
			const BSAnimationGraphEvent& a_event,
			BSTEventSource<BSAnimationGraphEvent>* a_source)
		{
			if (grenadeForm && a_event.tag == "staggerStop") {
				grenadeForm = nullptr;
				playerControls->meleeThrowHandler->buttonHoldDebounce = false;
			}
			return original(a_sink, a_event, a_source);
		}

		static void Install()
		{
			auto* sink = static_cast<BSTEventSink<BSAnimationGraphEvent>*>(player);
			auto* vtable = *reinterpret_cast<std::uintptr_t**>(sink);
			original = vtable[1];
			REL::WriteSafeData(
				reinterpret_cast<std::uintptr_t>(std::addressof(vtable[1])),
				reinterpret_cast<std::uintptr_t>(Thunk));
		}
	};

	void InitializePlugin()
	{
		player = PlayerCharacter::GetSingleton();
		playerControls = PlayerControls::GetSingleton();
		throwDelay = INISettingCollection::GetSingleton()->GetSetting("fThrowDelay:Controls");
		if (!throwDelay) {
			REX::ERROR("fThrowDelay:Controls was not found");
			return;
		}

		AnimationGraphHook::Install();
		CookInputHook::Install();
		GrenadeProjectileHook::Install();
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_message)
	{
		if (a_message->type == F4SE::MessagingInterface::kGameDataReady) {
			InitializePlugin();
		} else if (a_message->type == F4SE::MessagingInterface::kPreLoadGame ||
			a_message->type == F4SE::MessagingInterface::kNewGame) {
			grenadeForm = nullptr;
		}
	}
}

F4SEPluginLoad(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, {
		.log = true,
		.logName = "CookableGrenade",
	});
	taskInterface = F4SE::GetTaskInterface();
	const auto isOG = REX::FModule::IsRuntimeOG();
	const auto executableVersion = REX::FModule::GetExecutingModule().GetFileVersion();
	REX::INFO("detected Fallout 4 runtime={} f4seRuntimeVersion={} executableVersion={}",
		isOG ? "OG" : "AE", a_f4se->RuntimeVersion().string(), executableVersion.string());
	F4SE::GetMessagingInterface()->RegisterListener(OnF4SEMessage);
	return true;
}

extern "C"
{
	F4SE_EXPORT bool F4SEPlugin_Query(const F4SE::QueryInterface*, F4SE::PluginInfo* a_info)
	{
		const auto* versionData = F4SE::PluginVersionData::GetSingleton();
		if (!versionData) {
			return false;
		}
		a_info->name = versionData->GetPluginName().data();
		a_info->infoVersion = F4SE::PluginInfo::kVersion;
		a_info->version = versionData->pluginVersion;
		return true;
	}
}
