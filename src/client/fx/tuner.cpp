// Luanti "fx" fork: the in-game mood tuner (pause menu → Mood)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A formspec of sliders over the TUNE entries of params.def. Sliders write
// the fx_* settings directly, so the tuner is just another view of the
// regular settings (Settings → Mood Renderer shows the same values).

#include "renderer.h"

#include "gettext.h"
#include "settings.h"
#include "util/string.h"
#include <algorithm>
#include <sstream>

namespace fx
{

static std::vector<const ParamInfo *> sliders()
{
	std::vector<const ParamInfo *> out;
	for (const auto &i : paramInfo())
		if (i.tune)
			out.push_back(&i);
	return out;
}

static int sliderPos(const ParamInfo &p, float v)
{
	return core::round32(std::clamp((v - p.min) / (p.max - p.min), 0.f, 1.f) * 1000.f);
}

std::string tunerFormspec(const std::string &status)
{
	const auto params = sliders();
	const auto moods = listMoods();
	const std::string current = g_settings->get("fx_mood");
	const int per_col = (int)(params.size() + 1) / 2;
	const float row = 0.72f;
	const float h = 2.3f + per_col * row + 1.0f;

	std::ostringstream os;
	os << std::fixed;
	os.precision(2);
	os << "formspec_version[6]size[17," << h << "]"
		<< "bgcolor[#00000080;neither]"
		<< "label[0.4,0.5;" << strgettext("Mood tuner") << "]"
		<< "label[0.4,1.15;" << strgettext("Mood:") << "]";
	std::string list;
	int index = 1;
	for (size_t i = 0; i < moods.size(); i++) {
		std::string esc = moods[i];
		str_formspec_escape(esc);
		list += (i ? "," : "") + esc;
		if (moods[i] == current)
			index = (int)i + 1;
	}
	std::string note = tunerNote();
	if (note.empty())
		note = strgettext("Cave, night and underwater targets blend in automatically.");
	str_formspec_escape(note);
	os << "dropdown[1.6,0.85;4,0.6;fx_mood_select;" << list << ";" << index << ";false]"
		<< "label[6,1.15;" << note << "]";

	os << "scrollbaroptions[min=0;max=1000;smallstep=10;largestep=100;arrows=hide]";
	for (size_t i = 0; i < params.size(); i++) {
		const ParamInfo &p = *params[i];
		const int col = (int)i / per_col, r = (int)i % per_col;
		const float x = 0.4f + col * 8.3f, y = 1.9f + r * row;
		const float v = g_settings->getFloat(p.setting);
		os << "label[" << x << "," << (y + 0.25f) << ";" << p.label << " (" << v << ")]"
			<< "scrollbar[" << (x + 4.1f) << "," << y << ";3.8,0.45;horizontal;"
			<< p.setting << ";" << sliderPos(p, v) << "]";
	}
	const float yb = h - 0.95f;
	std::string esc_status = status;
	str_formspec_escape(esc_status);
	os << "field[0.4," << yb << ";4,0.65;fx_save_name;;" << current << "]"
		<< "field_close_on_enter[fx_save_name;false]"
		<< "button[4.5," << yb << ";3,0.65;fx_save;" << strgettext("Save as mood") << "]"
		<< "label[7.8," << (yb + 0.33f) << ";" << esc_status << "]"
		<< "button_exit[13.6," << yb << ";3,0.65;fx_done;" << strgettext("Done") << "]";
	return os.str();
}

bool tunerFields(const std::unordered_map<std::string, std::string> &fields, std::string &status)
{
	bool refresh = false;
	for (const ParamInfo *p : sliders()) {
		auto it = fields.find(p->setting);
		if (it == fields.end())
			continue;
		const std::string &val = it->second;
		const bool released = str_starts_with(val, "VAL:");
		if (!released && !str_starts_with(val, "CHG:"))
			continue;
		const int pos = mystoi(val.substr(4));
		// every event reports every scrollbar: only write the ones that moved,
		// so values set elsewhere are not snapped to the slider's 1/1000 grid
		if (pos == sliderPos(*p, g_settings->getFloat(p->setting)))
			continue;
		g_settings->setFloat(p->setting, p->min + pos / 1000.f * (p->max - p->min));
		refresh |= released;
	}
	if (auto it = fields.find("fx_mood_select"); it != fields.end() &&
			it->second != g_settings->get("fx_mood")) {
		g_settings->set("fx_mood", it->second);
		status = strgettext("Mood applied");
		refresh = true;
	}
	if (fields.count("fx_save")) {
		auto it = fields.find("fx_save_name");
		const std::string path = it == fields.end() ? "" : saveMood(it->second);
		status = path.empty() ? strgettext("Could not save (name must be letters, digits, _ or -)")
			: strgettext("Saved to ") + path;
		refresh = true;
	}
	return refresh;
}

} // namespace fx
