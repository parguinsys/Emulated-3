// vst3_host_test: load EIII Rack.vst3 and drive it the way a DAW does, without a DAW: instantiate, set up, activate,
// process blocks of MIDI in real time, read the output level, save and restore the state, shut down.
//   vst3_host_test PATH.vst3 [--seconds N] [--no-inputs] [--note-at S]
// --no-inputs passes no audio input buses at all (numInputs = 0, inputs = nullptr), as Qtractor does for plugins whose
// only input is an aux bus (here: Sample In).  Media come from the usual places (EIII_ROM_DIR / EIII_HD / EIII_CD or
// eiii.conf); without them the instrument stays silent, which still exercises every host call.
// Exit status 0 = no host call failed; the last line reports the output peak after the note.
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

int g_failures = 0;
bool check(bool ok, char const *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failures;
	return ok;
}

// logs restartComponent() and refuses re-entrant calls into the component, like a careful host
struct handler : IComponentHandler
{
	int restarts = 0;
	tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
	tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
	tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
	tresult PLUGIN_API restartComponent(int32 flags) override { ++restarts; printf("     restartComponent(0x%x)\n", unsigned(flags)); return kResultOk; }
	tresult PLUGIN_API queryInterface(TUID const iid, void **obj) override
	{
		if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) { *obj = this; return kResultOk; }
		*obj = nullptr; return kNoInterface;
	}
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
};

} // namespace

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc < 2) { fprintf(stderr, "usage: %s PATH.vst3 [--seconds N] [--no-inputs] [--note-at S]\n", argv[0]); return 2; }
	double seconds = 3, note_at = 1;
	bool no_inputs = false;
	for (int i = 2; i < argc; ++i)
	{
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
		else if (!strcmp(argv[i], "--note-at") && i + 1 < argc) note_at = atof(argv[++i]);
		else if (!strcmp(argv[i], "--no-inputs")) no_inputs = true;
	}

	std::string err;
	auto module = VST3::Hosting::Module::create(argv[1], err);
	if (!check(module != nullptr, "load module")) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
	auto const &factory = module->getFactory();
	VST3::Hosting::ClassInfo info;
	bool found = false;
	for (auto const &ci : factory.classInfos())
		if (ci.category() == kVstAudioEffectClass) { info = ci; found = true; break; }
	if (!check(found, "find audio module class")) return 1;
	printf("     %s  cid %s  %s\n", info.name().c_str(), info.ID().toString().c_str(), info.subCategoriesString().c_str());

	auto host = owned(new HostApplication);
	auto component = factory.createInstance<IComponent>(info.ID());
	if (!check(component != nullptr, "create component")) return 1;
	check(component->initialize(host) == kResultOk, "initialize");
	FUnknownPtr<IAudioProcessor> processor(component);
	FUnknownPtr<IEditController> controller(component);
	if (!check(processor && controller, "single component: processor + controller")) return 1;
	handler h;
	controller->setComponentHandler(&h);

	int32 const nin = component->getBusCount(kAudio, kInput), nout = component->getBusCount(kAudio, kOutput);
	for (int32 i = 0; i < nin; ++i)
	{
		BusInfo b; component->getBusInfo(kAudio, kInput, i, b);
		printf("     audio in %d: %d ch, %s\n", int(i), int(b.channelCount), b.busType == kMain ? "main" : "aux");
	}
	check(nout == 1, "one audio output bus");
	for (int32 i = 0; i < nin; ++i) component->activateBus(kAudio, kInput, i, !no_inputs);
	component->activateBus(kAudio, kOutput, 0, true);
	component->activateBus(kEvent, kInput, 0, true);

	// MIDI CC 1 (mod wheel) and pitch bend arrive as parameters through IMidiMapping
	ParamID mod_id = kNoParamId, bend_id = kNoParamId;
	if (FUnknownPtr<IMidiMapping> mm(controller); mm)
	{
		mm->getMidiControllerAssignment(0, 0, 1, mod_id);
		mm->getMidiControllerAssignment(0, 0, kPitchBend, bend_id);
	}
	check(mod_id != kNoParamId && bend_id != kNoParamId, "MIDI CC 1 and pitch bend are mapped");

	double const rate = 48000;
	int32 const block = 512;
	ProcessSetup setup{ kRealtime, kSample32, block, rate };
	check(processor->setupProcessing(setup) == kResultOk, "setupProcessing");
	check(component->setActive(true) == kResultOk, "setActive(true)");
	printf("     latency %u samples, %d restart(s) so far\n", unsigned(processor->getLatencySamples()), h.restarts);
	processor->setProcessing(true);

	std::vector<float> outl(block), outr(block), inl(block), inr(block);
	float *outs[2] = { outl.data(), outr.data() }, *ins[2] = { inl.data(), inr.data() };
	AudioBusBuffers out{}; out.numChannels = 2; out.channelBuffers32 = outs;
	AudioBusBuffers in{}; in.numChannels = 2; in.channelBuffers32 = ins;
	EventList events(64);
	ParameterChanges params(8);
	ProcessContext ctx{};
	ctx.sampleRate = rate; ctx.tempo = 120; ctx.state = ProcessContext::kTempoValid;

	long const blocks = long(seconds * rate / block), note_block = long(note_at * rate / block);
	float peak_after = 0;
	auto t0 = std::chrono::steady_clock::now();
	for (long b = 0; b < blocks; ++b)
	{
		events.clear(); params.clearQueue();
		Event e{};
		e.busIndex = 0;
		if (b == note_block)
		{
			e.type = Event::kNoteOnEvent; e.noteOn.channel = 0; e.noteOn.pitch = 60; e.noteOn.velocity = 0.8f; e.noteOn.noteId = -1;
			events.addEvent(e);
			int32 idx;
			if (auto *q = params.addParameterData(mod_id, idx)) q->addPoint(0, 64 / 127.0, idx);
			if (auto *q = params.addParameterData(bend_id, idx)) q->addPoint(0, 0.75, idx);
		}
		if (b == note_block + long(0.5 * rate / block))
		{
			e.type = Event::kNoteOffEvent; e.noteOff.channel = 0; e.noteOff.pitch = 60; e.noteOff.velocity = 0.f; e.noteOff.noteId = -1;
			events.addEvent(e);
		}
		ProcessData d{};
		d.processMode = kRealtime; d.symbolicSampleSize = kSample32; d.numSamples = block;
		d.numInputs = no_inputs ? 0 : nin; d.inputs = no_inputs ? nullptr : &in;
		d.numOutputs = 1; d.outputs = &out;
		d.inputEvents = &events; d.inputParameterChanges = &params; d.processContext = &ctx;
		std::fill(outl.begin(), outl.end(), 0.f); std::fill(outr.begin(), outr.end(), 0.f);
		if (processor->process(d) != kResultOk) { check(false, "process"); break; }
		ctx.projectTimeSamples += block;
		if (b >= note_block)
			for (int32 i = 0; i < block; ++i) peak_after = std::max({ peak_after, std::fabs(outl[i]), std::fabs(outr[i]) });
		std::this_thread::sleep_until(t0 + std::chrono::microseconds(long((b + 1) * block * 1e6 / rate)));
	}
	printf("ok   processed %ld blocks of %d (%s)\n", blocks, int(block), no_inputs ? "no input buses" : "with Sample In");

	// state round trip, as on project save / reopen
	auto state = owned(new MemoryStream);
	check(component->getState(state) == kResultOk, "getState");
	int64 size = 0; state->seek(0, IBStream::kIBSeekEnd, &size); state->seek(0, IBStream::kIBSeekSet, nullptr);
	printf("     state %lld bytes\n", (long long)size);
	check(component->setState(state) == kResultOk, "setState");

	processor->setProcessing(false);
	check(component->setActive(false) == kResultOk, "setActive(false)");
	controller->setComponentHandler(nullptr);
	check(component->terminate() == kResultOk, "terminate");
	printf("peak after note: %.4f (%s)\n", peak_after, peak_after > 1e-4f ? "sound" : "silent: no media, or the OS has not booted yet");
	return g_failures ? 1 : 0;
}
