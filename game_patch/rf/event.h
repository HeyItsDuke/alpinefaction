#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include <cstddef>
#include "object.h"
#include "level.h"
#include "misc.h"
#include "multi.h"
#include "hud.h"
#include "entity.h"
#include "trigger.h"
#include "../main/main.h"
#include "player/player.h"
#include "os/timestamp.h"

namespace rf
{
    struct Event : Object
    {
        int event_type;
        float delay_seconds;
        Timestamp delay_timestamp;
        VArray<int> links;
        int triggered_by_handle;
        int trigger_handle;
        int event_flags;
        bool delayed_msg;

        // handler storage, defined in event.cpp
        static std::unordered_map<const Event*, std::unordered_map<std::string, std::function<void(Event*, const std::string&)>>>
            variable_handler_storage;

        // register variable handlers (AF new) plus default event initialization (does nothing)
        // safe to override, but include call to base struct initialize for var handler registration
        virtual void initialize()
        {
            register_variable_handlers(); // possibly codeinjection this into the stock init function?
            AddrCaller{0x004B8CD0}.this_call(this);
        };

        // default event turning on (switch based on some default event types)
        // safe to override
        virtual void turn_on()
        {
            AddrCaller{0x004B9070}.this_call(this);
        };

        // default event turning off (switch based on some default event types)
        // safe to override
        virtual void turn_off()
        {
            AddrCaller{0x004B9F80}.this_call(this);
        };

        // default event processing, handles delays (set in activate) and switch on some default event types
        // if overridden, delays need to be handled otherwise event won't work with delay specified (just like some stock events)
        virtual void process()
        {
            AddrCaller{0x004B8CE0}.this_call(this);
        };

        // game does not allocate - only usable in new code
        virtual void activate(int trigger_handle, int triggered_by_handle, bool on)
        {
            do_activate(trigger_handle, triggered_by_handle, on);
        }

        // game does not allocate - only usable in new code
        virtual void activate_links(int trigger_handle, int triggered_by_handle, bool on)
        {
            do_activate_links(trigger_handle, triggered_by_handle, on);
        }

    protected:
        // default internal event activation, handles delays, turn on/off, and event forwarding
        // if overridden, delays need to be handled otherwise event won't work with delay specified
        virtual void do_activate(int trigger_handle, int triggered_by_handle, bool on)
        {
            AddrCaller{0x004B8B70}.this_call(this, trigger_handle, triggered_by_handle, on);
        }

        // default event link activation, handles sending on/off signals to links
        // safe to override, but be careful - this is standard behaviour for all stock events
        virtual void do_activate_links(int trigger_handle, int triggered_by_handle, bool on)
        {
            AddrCaller{0x004B8B00}.this_call(this, trigger_handle, triggered_by_handle, on);
        }

        virtual void register_variable_handlers()
        {
            auto& handlers = variable_handler_storage[this];
            handlers["delay"] = [](Event* event, const std::string& value) {
                event->delay_seconds = std::stof(value);
                xlog::warn("apply_var: delay set to {}", event->delay_seconds);
            };
        }

    public:
        virtual void apply_var(const std::string& var_name, const std::string& value)
        {
            auto it = variable_handler_storage.find(this);
            if (it != variable_handler_storage.end()) {
                auto& handlers = it->second;
                auto handler_it = handlers.find(var_name);
                if (handler_it != handlers.end()) {
                    try {
                        handler_it->second(this, value);
                    }
                    catch (const std::exception& ex) {
                        xlog::error("apply_var: Failed to set var_name={} with value={} - {}", var_name, value,
                                    ex.what());
                    }
                }
                else {
                    xlog::warn("apply_var: Unsupported var_name={} for Event", var_name);
                }
            }
            else {
                xlog::warn("apply_var: No handlers registered for Event");
            }
        }
    };
    static_assert(sizeof(Event) == 0x2B8); // 0x2B5 in original code

    struct GenericEvent : Event
    {
        char event_specific_data[21]; // 24 in original code, adjust per new event struct size
    };
    static_assert(sizeof(GenericEvent) == 0x2D0, "GenericEvent size is incorrect!");

    struct PersistentGoalEvent
    {
        rf::String name;
        int initial_count;
        int count;
    };
    static_assert(sizeof(PersistentGoalEvent) == 0x10);

    /* struct EventGoalCreate : Event
    {
        int initial_count;                                    // Offset 0x2B5
        int complete_count_unused;                            // Offset 0x2B9
        int count;                                            // Offset 0x2BD
        char is_persistent;                                   // Offset 0x2C1
    };
    static_assert(sizeof(EventGoalCreate) == 0x2C6, "EventGoalCreate struct size mismatch!");*/



    static auto& event_lookup_from_uid = addr_as_ref<Event*(int uid)>(0x004B6820);
    static auto& event_lookup_from_handle = addr_as_ref<Event*(int handle)>(0x004B6800);
    static auto& event_create = addr_as_ref<Event*(const rf::Vector3* pos, int event_type)>(0x004B6870);
    static auto& event_delete = addr_as_ref<void(rf::Event*)>(0x004B67C0);
    static auto& event_add_link = addr_as_ref<void(int event_handle, int handle)>(0x004B6790);
    static auto& event_find_named_event = addr_as_ref<GenericEvent*(String* name)>(0x004BD740);
    static auto& event_lookup_persistent_goal_event = addr_as_ref<PersistentGoalEvent*(const char* name)>(0x004B8680);


    // custom event structs
    // id 90
    struct EventSetVar : Event
    {
        std::string var_name;
        std::optional<std::string> var_value;

        void turn_on() override
        {
            this->activate(this->trigger_handle, this->triggered_by_handle, true);
        }

        void do_activate(int trigger_handle, int triggered_by_handle, bool on) override
        {
            if (var_name.empty()) {
                xlog::error("SetVar event with UID {} has no var_name defined!", this->uid);
                return;
            }

            std::string value = var_value.value_or("");

            xlog::info("Activating event UID {} with var_name: '{}' and value: '{}'", this->uid, var_name, value);

            for (int link_handle : this->links) {
                rf::Object* obj = rf::obj_from_handle(link_handle);
                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);
                    try {
                        linked_event->apply_var(var_name, value);
                    }
                    catch (const std::exception& ex) {
                        xlog::error("Failed to apply var_name={} with value={} to linked event UID={} - {}", var_name,
                                    value, linked_event->uid, ex.what());
                    }
                }
            }
        }
    };

    // id 91
    struct EventCloneEntity : Event
    {
        void turn_on() override
        {
            xlog::warn("Turning on event UID {}", this->uid);

            for (int i = 0; i < this->links.size(); ++i) {

                int link = this->links[i];
                //xlog::warn("Link at index {}: {}", i, link);
                rf::Object* obj = rf::obj_from_handle(link);
                if (obj) {
                    rf::Entity* entity = static_cast<rf::Entity*>(obj);
                    //xlog::warn("Name: {}, UID: {}, type: {}, ent type: {}", entity->name.c_str(), entity->uid,
                    //           static_cast<int>(entity->type), entity->info_index);
                    rf::Entity* new_entity =
                        rf::entity_create(entity->info_index, entity->name, -1, pos, entity->orient, 0, -1);
                    new_entity->entity_flags = entity->entity_flags;
                    new_entity->pos = entity->pos;
                    new_entity->entity_flags2 = entity->entity_flags2;
                    new_entity->info = entity->info;
                    new_entity->info2 = entity->info2;
                    new_entity->drop_item_class = entity->drop_item_class;
                    new_entity->obj_flags = entity->obj_flags;
                    new_entity->ai.custom_attack_range = entity->ai.custom_attack_range;
                    new_entity->ai.use_custom_attack_range = entity->ai.use_custom_attack_range;
                    new_entity->ai.attack_style = entity->ai.attack_style;
                    new_entity->ai.cooperation = entity->ai.cooperation;
                    new_entity->ai.cover_style = entity->ai.cover_style;
                    for (int i = 0; i < 64; ++i) {
                        new_entity->ai.has_weapon[i] = entity->ai.has_weapon[i];
                        new_entity->ai.clip_ammo[i] = entity->ai.clip_ammo[i];
                    }

                    for (int i = 0; i < 32; ++i) {
                        new_entity->ai.ammo[i] = entity->ai.ammo[i];
                    }
                    new_entity->ai.current_secondary_weapon = entity->ai.current_secondary_weapon;
                    new_entity->ai.current_primary_weapon = entity->ai.current_primary_weapon;

                    //xlog::warn("Name: {}, UID: {}, type: {}, ent type: {}", new_entity->name.c_str(), new_entity->uid,
                    //           static_cast<int>(new_entity->type), new_entity->info_index);
                }
            }
        }
    };

    // id 92
    struct EventSetCollisionPlayer : Event
    {
        void turn_on() override
        {
            xlog::warn("Turning on event UID {}", this->uid);
            rf::local_player->collides_with_world = true;
        }
        void turn_off() override
        {
            xlog::warn("Turning off event UID {}", this->uid);
            rf::local_player->collides_with_world = false;
        }
    };

    // id 93
    struct EventSwitchRandom : Event
    {
        void turn_on() override
        {
            xlog::warn("Turning on event UID {}", this->uid);

            if (this->links.size() > 0) {
                // select a random index from links
                std::uniform_int_distribution<int> dist(0, this->links.size() - 1);
                int random_index = dist(g_rng);
                int link_handle = this->links[random_index];

                rf::Object* obj = rf::obj_from_handle(link_handle);
                if (obj) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);
                    if (linked_event) {
                        // Send the "turn on" message
                        linked_event->turn_on();
                        xlog::warn("Randomly selected event UID {} and turned it on.", linked_event->uid);
                    }
                }
            }
            else {
                xlog::warn("Event UID {} has no links to turn on.", this->uid);
            }

        }
    };

    // id 94
    struct EventDifficultyGate : Event
    {
        rf::GameDifficultyLevel difficulty = GameDifficultyLevel::DIFFICULTY_EASY;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["difficulty"] = [](Event* event, const std::string& value) {
                auto* gate_event = static_cast<EventDifficultyGate*>(event);
                int difficulty_value = std::stoi(value);
                gate_event->difficulty = static_cast<rf::GameDifficultyLevel>(difficulty_value);
                xlog::warn("apply_var: Set difficulty to {} for EventDifficultyGate", difficulty_value);
            };
        }

        void turn_on() override
        {
            xlog::warn("Gate {} with UID {} is checking for difficulty {}",
                this->name, this->uid, static_cast<int>(difficulty));

            if (rf::game_get_skill_level() == difficulty) {
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
        }

        void turn_off() override
        {
            xlog::warn("Gate {} with UID {} is checking for difficulty {}",
                this->name, this->uid, static_cast<int>(difficulty));

            if (rf::game_get_skill_level() == difficulty) {
                activate_links(this->trigger_handle, this->triggered_by_handle, false);
            }
        }
    };

    // id 95
    struct EventHUDMessage : Event
    {
        std::string message = "";
        std::optional<int> duration;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["message"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventHUDMessage*>(event);
                this_event->message = value;
                xlog::warn("apply_var: Set message to '{}' for EventHUDMessage UID={}", this_event->message,
                           this_event->uid);
            };

            handlers["duration"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventHUDMessage*>(event);
                this_event->duration = std::stoi(value);
                xlog::warn("apply_var: Set duration to '{}' for EventHUDMessage UID={}",
                           this_event->duration.value_or(0), this_event->uid);
            };            
        }

        void turn_on() override
        {
            rf::hud_msg(message.c_str(), 0, duration.value_or(0), 0);
        }

        void turn_off() override
        {
            rf::hud_msg_clear();
        }
    };

    // id 96
    struct EventPlayVideo : Event
    {
        std::optional<std::string> filename;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["filename"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventPlayVideo*>(event);
                this_event->filename = value;
                xlog::warn("apply_var: Set message to '{}' for EventPlayVideo UID={}", this_event->filename->c_str(),
                           this_event->uid);
            };            
        }

        void turn_on() override
        {
            rf::bink_play(filename->c_str());
        }
    };

    // id 97
    struct EventSetLevelHardness : Event
    {
        int hardness;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["hardness"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetLevelHardness*>(event);
                this_event->hardness = std::clamp(std::stoi(value), 0, 100);
                xlog::warn("apply_var: Set hardness to {} for EventSetLevelHardness UID={}", this_event->hardness,
                            this_event->uid);
            };
        }

        void turn_on() override
        {
            xlog::warn("level hardness is currently {}, setting to {}", rf::level.default_rock_hardness, hardness);
            rf::level.default_rock_hardness = std::clamp(hardness, 0, 100);
        }
    };

    // id 98
    struct EventSequence : Event
    {
        int last_link_index = -1; // start with firing index 0

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["last_link_index"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSequence*>(event);
                this_event->last_link_index = std::stoi(value);
                xlog::warn("apply_var: Set last_link_index to {} for EventSequence UID={}", this_event->last_link_index,
                           this_event->uid);
            };
        }

        void turn_on() override
        {
            xlog::warn("Turning on event UID {}", this->uid);

            if (this->links.empty()) {
                xlog::warn("Event UID {} has no links to turn on.", this->uid);
                return;
            }

            // if last_link_index is out of bounds (for example, if a linked object was destroyed), start at 0
            if (last_link_index < 0 || last_link_index >= static_cast<int>(this->links.size())) {
                last_link_index = 0;
            }
            else {
                // tick up last_link_index, wrap around if at the end of the links array
                last_link_index = (last_link_index + 1) % this->links.size();
            }

            // find link to be activated, activate it
            int link_handle = this->links[last_link_index];
            rf::Object* obj = rf::obj_from_handle(link_handle);
            if (obj && obj->type == OT_EVENT) {
                rf::Event* linked_event = static_cast<rf::Event*>(obj);
                linked_event->turn_on();
                xlog::warn("Activated event UID {} at index {}.", linked_event->uid, last_link_index);
            }
        }
    };

    // id 99
    struct EventClearQueued : Event
    {
        void turn_on() override
        {
            for (size_t i = 0; i < this->links.size(); ++i) {
                int link_handle = this->links[i];
                rf::Object* obj = rf::obj_from_handle(link_handle);

                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);
                    linked_event->delay_timestamp.invalidate();
                    linked_event->delayed_msg = 0;
                    xlog::warn("Cleared queue for event UID {} at index {}.", linked_event->uid, i);
                }                
            }
        }
    };

    // id 100
    struct EventRemoveLink : Event
    {
        bool remove_all = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["remove_all"] = [](Event* event, const std::string& value) {
                auto* gate_event = static_cast<EventRemoveLink*>(event);
                gate_event->remove_all = (value == "true");
                xlog::warn("apply_var: Set remove_all to {}", gate_event->remove_all);
            };
        }

        void turn_on() override
        {
            xlog::warn("Turning on EventRemoveLink UID {}", this->uid);

            for (size_t i = 0; i < this->links.size(); ++i) {
                int link_handle = this->links[i];
                rf::Object* obj = rf::obj_from_handle(link_handle);

                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);

                    if (remove_all) {
                        linked_event->links.clear();
                        xlog::warn("Removed all links from event UID {} linked at index {}.", linked_event->uid, i);
                    }
                    else {
                        // Remove only the links between events in this->links
                        linked_event->links.erase_if([this](int inner_link_handle) {
                            return std::find(this->links.begin(), this->links.end(), inner_link_handle) !=
                                   this->links.end();
                        });
                        xlog::warn("Removed links between EventRemoveLink UID {} and other linked events.", this->uid);
                    }
                }
                else {
                    xlog::warn("Invalid or non-event object at link index {}.", i);
                }
            }
        }
    };

    // id 101
    struct EventFixedDelay : Event {}; // no allocations needed

    // id 102
    struct EventAddLink : Event
    {
        void turn_on() override
        {
            xlog::warn("Turning on EventAddLink UID {}", this->uid);

            if (this->links.empty()) {
                xlog::warn("Event UID {} has no links to process.", this->uid);
                return;
            }

            int source_link_handle = this->links[0];

            for (size_t i = 1; i < this->links.size(); ++i) {
                int target_link_handle = this->links[i];
                event_add_link(source_link_handle, target_link_handle);
                xlog::warn("Added link from source UID {} to target UID {}.", source_link_handle, target_link_handle);
            }
        }
    };

    // id 103
    struct EventValidGate : Event
    {
        int check_uid = -1;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["check_uid"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventValidGate*>(event);
                this_event->check_uid = std::stoi(value);
                xlog::warn("apply_var: Set check_uid to {} for EventValidGate UID={}", this_event->check_uid,
                           this_event->uid);
            };
        }

        void turn_on() override
        {
            rf::Object* obj = rf::obj_lookup_from_uid(check_uid);

            if (check_uid == -1 ||
                !obj ||
                obj->type == rf::OT_CORPSE ||
                (obj->type == rf::OT_ENTITY && rf::entity_from_handle(obj->handle)->life <= 0)) { // if entity, alive
                return;
            }

            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }

        void turn_off() override
        {
            rf::Object* obj = rf::obj_lookup_from_uid(check_uid);

            if (check_uid == -1 || !obj || obj->type == rf::OT_CORPSE ||
                (obj->type == rf::OT_ENTITY && rf::entity_from_handle(obj->handle)->life <= 0)) { // if entity, alive
                return;
            }

            activate_links(this->trigger_handle, this->triggered_by_handle, false);
        }
    };

    // id 104
    struct EventGoalMath : Event
    {
        std::optional<std::string> goal;
        std::optional<std::string> operation;
        std::optional<int> value;
        std::optional<int> value2;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["goal"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->goal = value;
                xlog::warn("apply_var: Set goal to '{}' for EventGoalMath UID={}", this_event->goal->c_str(),
                           this_event->uid);
            };

            handlers["operation"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->operation = value;
                xlog::warn("apply_var: Set operation to '{}' for EventGoalMath UID={}", this_event->operation->c_str(),
                           this_event->uid);
            };

            handlers["value"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->value = std::stoi(value);
                xlog::warn("apply_var: Set value to {} for EventGoalMath UID={}", this_event->value.value_or(1),
                           this_event->uid);
            };

            handlers["value2"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->value2 = std::stoi(value);
                xlog::warn("apply_var: Set value2 to {} for EventGoalMath UID={}", this_event->value2.value_or(1),
                           this_event->uid);
            };
        }

        void turn_on() override
        {
            xlog::warn("Turning on EventGoalMath UID {}", this->uid);

            if (!goal.has_value() || !operation.has_value()) {
                return;
            }

            int effective_value = value.value_or(1); // Default value of 1 if not specified
            int effective_value2 = value2.value_or(effective_value); // Default to same as value if not specified
            String effective_goal = goal.value_or("").c_str();


            // find level goal
            GenericEvent* named_event = event_find_named_event(&effective_goal);

            int* goal_count_ptr = nullptr;
            int* goal_initial_ptr = nullptr;
            if (named_event) {
                goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]); // 11 in original code
                goal_initial_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[0]);
            }

            // find persistent goal
            PersistentGoalEvent* persist_event = event_lookup_persistent_goal_event(effective_goal);

            if (!persist_event && !named_event) {
                xlog::error("Did not find a level or persistent goal named '{}'", effective_goal);
                return;
            }

            if (goal_count_ptr) {
                xlog::warn("Level goal count before operation: {}, initial: {}", *goal_count_ptr, *goal_initial_ptr);
            }

            if (persist_event) {
                xlog::warn("Persistent goal '{}', count: {}, initial count: {}", persist_event->name,
                           persist_event->count, persist_event->initial_count);
            }

            // Determine the operation
            const std::string& op = operation.value();
            auto apply_operation = [&](int& goal_count, int initial_value) {
                if (op == "add") {
                    goal_count += effective_value;
                }
                else if (op == "sub") {
                    goal_count -= effective_value;
                }
                else if (op == "mul") {
                    goal_count *= effective_value;
                }
                else if (op == "div") {
                    if (effective_value != 0) {
                        goal_count /= effective_value;
                    }
                    else {
                        xlog::error("Division by zero attempted in EventGoalMath UID {}", this->uid);
                    }
                }
                else if (op == "rdiv") {
                    if (goal_count != 0) {
                        goal_count = effective_value / goal_count;
                    }
                    else {
                        xlog::error("Division by zero attempted in reverse divide operation for EventGoalMath UID {}",
                                    this->uid);
                    }
                }
                else if (op == "set") {
                    goal_count = effective_value;
                }
                else if (op == "mod") {
                    if (effective_value != 0) {
                        goal_count %= effective_value;
                    }
                    else {
                        xlog::error("Modulo by zero attempted in EventGoalMath UID {}", this->uid);
                    }
                }
                else if (op == "pow") {
                    if (effective_value >= 0) {
                        goal_count = static_cast<int>(std::pow(goal_count, effective_value));
                    }
                    else {
                        xlog::error("Negative exponent {} not allowed in EventGoalMath UID {}", effective_value,
                                    this->uid);
                    }
                }
                else if (op == "neg") {
                    goal_count = -goal_count;
                }
                else if (op == "abs") {
                    goal_count = std::abs(goal_count);
                }
                else if (op == "clamp") {
                    goal_count = std::clamp(goal_count, effective_value, effective_value2);
                }
                else if (op == "max") {
                    goal_count = std::max(goal_count, effective_value);
                }
                else if (op == "min") {
                    goal_count = std::min(goal_count, effective_value);
                }
                else if (op == "reset") {
                    goal_count = initial_value;
                }
                else {
                    xlog::error("Unknown operation '{}' in EventGoalMath UID {}", op, this->uid);
                }
            };

            // apply to persistent goal
            if (persist_event) {
                xlog::warn(
                    "Applying operation '{}' with value '{}', value2 '{}', to persistent goal '{}'", op, effective_value, effective_value2, persist_event->name);

                apply_operation(persist_event->count, persist_event->initial_count);
            }

            // apply to level goal
            if (goal_count_ptr) {
                xlog::warn("Applying operation '{}' with value '{}', value2 '{}', to level goal '{}'", op, effective_value,
                           effective_value2, effective_goal);

                apply_operation(*goal_count_ptr, *goal_initial_ptr);
            }

            // Log final values
            if (persist_event) {
                xlog::warn("Persistent goal '{}', new count: {}, initial count: {}", persist_event->name,
                           persist_event->count, persist_event->initial_count);
            }

            if (goal_count_ptr) {
                xlog::warn("Level goal count after operation: {}", *goal_count_ptr);
            }
        }
    };

    // id 105
    struct EventGoalGate : Event
    {
        std::optional<std::string> goal;
        std::optional<std::string> test_type;
        std::optional<int> value;
        std::optional<int> value2;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["goal"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->goal = value;
                xlog::warn("apply_var: Set goal to '{}' for EventGoalGate UID={}", this_event->goal->c_str(),
                           this_event->uid);
            };

            handlers["test_type"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->test_type = value;
                xlog::warn("apply_var: Set test_type to '{}' for EventGoalGate UID={}", this_event->test_type->c_str(),
                           this_event->uid);
            };

            handlers["value"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->value = std::stoi(value);
                xlog::warn("apply_var: Set value to {} for EventGoalGate UID={}", this_event->value.value_or(1),
                           this_event->uid);
            };

            handlers["value2"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->value2 = std::stoi(value);
                xlog::warn("apply_var: Set value2 to {} for EventGoalGate UID={}", this_event->value2.value_or(1),
                           this_event->uid);
            };
        }

        void turn_on() override
        {
            xlog::warn("Turning on EventGoalGate UID {}", this->uid);

            if (!goal.has_value() || !test_type.has_value()) {
                return;
            }

            int effective_value = value.value_or(0); // Default value to 0 if not specified
            int effective_value2 = value2.value_or(effective_value); // Default to same as value if not specified
            String effective_goal = goal.value_or("").c_str();

            // find level goal
            GenericEvent* named_event = event_find_named_event(&effective_goal);

            int* goal_count_ptr = nullptr;
            int* goal_initial_ptr = nullptr;
            if (named_event) {
                goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]); // 11 in original code
                goal_initial_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[0]);
            }

            // find persistent goal
            PersistentGoalEvent* persist_event = event_lookup_persistent_goal_event(effective_goal);

            if (!persist_event && !named_event) {
                xlog::error("Did not find a level or persistent goal named '{}'", effective_goal);
                return;
            }

            int current_value = 0;
            int initial_value = 0;
            if (goal_count_ptr) {
                current_value = *goal_count_ptr;
                initial_value = goal_initial_ptr ? *goal_initial_ptr : 0;
            }
            else if (persist_event) {
                current_value = persist_event->count;
                initial_value = persist_event->initial_count;
            }

            xlog::warn("Current goal value for '{}': {}, Initial value: {}", effective_goal, current_value,
                       initial_value);

            const std::string& test = test_type.value();
            bool pass = false;


            if (test == "equal") {
                pass = (current_value == effective_value);
            }
            else if (test == "nequal") {
                pass = (current_value != effective_value);
            }
            else if (test == "gt") {
                pass = (current_value > effective_value);
            }
            else if (test == "lt") {
                pass = (current_value < effective_value);
            }
            else if (test == "geq") {
                pass = (current_value >= effective_value);
            }
            else if (test == "leq") {
                pass = (current_value <= effective_value);
            }
            else if (test == "between") {
                pass = (current_value >= effective_value && current_value <= effective_value2);
            }
            else if (test == "odd") {
                pass = (current_value % 2 != 0);
            }
            else if (test == "even") {
                pass = (current_value % 2 == 0);
            }
            else if (test == "divisible") {
                if (effective_value != 0) {
                    pass = (current_value % effective_value == 0);
                }
                else {
                    xlog::error("Division by zero in divisible test for EventGoalGate UID {}", this->uid);
                }
            }
            else if (test == "ltinit") {
                pass = (current_value < initial_value);
            }
            else if (test == "gtinit") {
                pass = (current_value > initial_value);
            }
            else if (test == "leinit") {
                pass = (current_value <= initial_value);
            }
            else if (test == "geinit") {
                pass = (current_value >= initial_value);
            }
            else if (test == "eqinit") {
                pass = (current_value == initial_value);
            }
            else {
                xlog::error("Unknown test type '{}' in EventGoalGate UID {}", test, this->uid);
            }

            // Log test result
            if (pass) {
                xlog::warn("Test '{}' passed for EventGoalGate UID {}", test, this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
            else {
                xlog::warn("Test '{}' failed for EventGoalGate UID {}", test, this->uid);
            }
        }
    };

    // id 106
    struct EventEnvironmentGate : Event
    {
        std::optional<std::string> environment;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["environment"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventEnvironmentGate*>(event);
                this_event->environment = value;
                xlog::warn("apply_var: Set environment to '{}' for EventEnvironmentGate UID={}", this_event->environment->c_str(),
                           this_event->uid);
            };
        }

        void turn_on() override
        {
            xlog::warn("Turning on EventEnvironmentGate UID {}", this->uid);

            if (!environment.has_value()) {
                return;
            }

            const std::string& test = environment.value();
            bool pass = false;

            if (test == "multi") {
                pass = rf::is_multi;
            }
            else if (test == "single") {
                pass = !rf::is_multi;
            }
            else if(test == "server") {
                pass = (rf::is_server || rf::is_dedicated_server);
            }
            else if(test == "dedicated") {
                pass = rf::is_dedicated_server;
            }
            else if (test == "client") {
                pass = (!rf::is_server && !rf::is_dedicated_server);
            }
            else {
                xlog::error("Unknown environment test '{}' for EventEnvironmentGate UID {}", test, this->uid);
            }

            if (pass) {
                xlog::warn("Test '{}' passed for EventEnvironmentGate UID {}", test, this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
            else {
                xlog::warn("Test '{}' failed for EventEnvironmentGate UID {}", test, this->uid);
            }
        }
    };

    // id 107
    struct EventInsideGate : Event
    {
        int check_uid = -1;
        std::optional<int> test_uid;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["check_uid"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventInsideGate*>(event);
                this_event->check_uid = std::stoi(value);
                xlog::warn("apply_var: Set check_uid to {} for EventInsideGate UID={}", this_event->check_uid,
                           this_event->uid);
            };

            handlers["test_uid"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventInsideGate*>(event);
                this_event->test_uid = std::stoi(value);
                xlog::warn("apply_var: Set test_uid to {} for EventInsideGate UID={}", this_event->test_uid.value_or(-1),
                           this_event->uid);
            };
        }

        void turn_on() override
        {
            Object* obj = obj_lookup_from_uid(check_uid);
            GRoom* room = level_room_from_uid(check_uid);

            int obj_handle_to_test = -1;

            if (test_uid.has_value() && test_uid.value() > 0) {
                xlog::warn("test_uid has value");
                Object* obj_to_test = obj_lookup_from_uid(test_uid.value_or(-1));

                if (obj_to_test) {
                    xlog::warn("test_uid is using {}", obj_to_test->name);
                    obj_handle_to_test = obj_to_test->handle;
                }
            }
            else {
                obj_handle_to_test = triggered_by_handle;
            }

            Object* triggered_by_obj = obj_from_handle(obj_handle_to_test);            

            if (check_uid == -1 || !triggered_by_obj || (!obj && !room)) {
                return;
            }

            xlog::warn("object being tested is {}, UID {}", triggered_by_obj->name, triggered_by_obj->uid);

            bool pass = false;

            if (obj && obj->type == OT_TRIGGER) {
                Trigger* trigger = static_cast<Trigger*>(obj);

                xlog::warn("Trigger UID {} is type {}", check_uid, trigger->type);

                switch (trigger->type) {
                case 0: // sphere shape trigger
                    pass = trigger_inside_bounding_sphere(trigger, triggered_by_obj);
                    break;

                case 1: // box shape trigger
                    pass = trigger_inside_bounding_box(trigger, triggered_by_obj);
                    break;

                default:
                    xlog::warn("Unknown trigger type {} for EventInsideGate UID {}", trigger->type, this->uid);
                }
            }
            else if (room) { // handle room test
                xlog::warn("tested object's room is {}, test room is {}", triggered_by_obj->room->uid, room->uid);
                if (room && triggered_by_obj->room == room) {
                    pass = true;
                }
            }

            if (pass) {
                xlog::warn("Test passed for EventInsideGate UID {}", this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
            else {
                xlog::warn("Test failed for EventInsideGate UID {}", this->uid);
            }
        }
    };

    enum class EventType : int
    {
        Attack = 1,
        Bolt_State,
        Continuous_Damage,
        Cyclic_Timer,
        Drop_Point_Marker,
        Explode,
        Follow_Player,
        Follow_Waypoints,
        Give_Item_To_Player,
        Goal_Create,
        Goal_Check,
        Goal_Set,
        Goto,
        Goto_Player,
        Heal,
        Invert,
        Load_Level,
        Look_At,
        Make_Invulnerable,
        Make_Fly,
        Make_Walk,
        Message,
        Music_Start,
        Music_Stop,
        Particle_State,
        Play_Animation,
        Play_Sound,
        Slay_Object,
        Remove_Object,
        Set_AI_Mode,
        Set_Light_State,
        Set_Liquid_Depth,
        Set_Friendliness,
        Shake_Player,
        Shoot_At,
        Shoot_Once,
        Armor,
        Spawn_Object,
        Swap_Textures,
        Switch,
        Switch_Model,
        Teleport,
        When_Dead,
        Set_Gravity,
        Alarm,
        Alarm_Siren,
        Go_Undercover,
        Delay,
        Monitor_State,
        UnHide,
        Push_Region_State,
        When_Hit,
        Headlamp_State,
        Item_Pickup_State,
        Cutscene,
        Strip_Player_Weapons,
        Fog_State,
        Detach,
        Skybox_State,
        Force_Monitor_Update,
        Black_Out_Player,
        Turn_Off_Physics,
        Teleport_Player,
        Holster_Weapon,
        Holster_Player_Weapon,
        Modify_Rotating_Mover,
        Clear_Endgame_If_Killed,
        Win_PS2_Demo,
        Enable_Navpoint,
        Play_Vclip,
        Endgame,
        Mover_Pause,
        Countdown_Begin,
        Countdown_End,
        When_Countdown_Over,
        Activate_Capek_Shield,
        When_Enter_Vehicle,
        When_Try_Exit_Vehicle,
        Fire_Weapon_No_Anim,
        Never_Leave_Vehicle,
        Drop_Weapon,
        Ignite_Entity,
        When_Cutscene_Over,
        When_Countdown_Reaches,
        Display_Fullscreen_Image,
        Defuse_Nuke,
        When_Life_Reaches,
        When_Armor_Reaches,
        Reverse_Mover,
        SetVar, // 90
        Clone_Entity,
        Set_Player_World_Collide,
        Switch_Random,
        Difficulty_Gate,
        HUD_Message,
        Play_Video,
        Set_Level_Hardness,
		Sequence,
        Clear_Queued,
		Remove_Link,
        Fixed_Delay,
        Add_Link,
        Valid_Gate,
        Goal_Math,
        Goal_Gate,
        Environment_Gate,
        Inside_Gate
    };

    // int to EventType
    inline EventType int_to_event_type(int id)
    {
        return static_cast<EventType>(id);
    }

    // EventType to int
    inline int event_type_to_int(EventType eventType)
    {
        return static_cast<int>(eventType);
    }
}
