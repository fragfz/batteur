#include "FileReadingHelpers.h"
#include "MathHelpers.h"
#include "MidiHelpers.h"
#include <algorithm>

tl::optional<unsigned> getQuarterPerBars(const fmidi_event_t& evt)
{
    if (evt.data[0] != 0x58)
        return {};

    return evt.data[1] / (1 << (evt.data[2] - 2));
}

tl::optional<double> getSecondsPerQuarter(const fmidi_event_t& evt)
{
    if (evt.data[0] != 0x51 || evt.datalen != 4)
        return {};

    const uint8_t* d24 = &evt.data[1];
    const uint32_t tempo = (d24[0] << 16) | (d24[1] << 8) | d24[2];
    return 1e-6 * tempo;
}

tl::expected<batteur::Sequence, ReadingError> readSequenceFromFile(nlohmann::json& json, const fs::path& rootDirectory)
{
    fs::path filepath = rootDirectory / json["filename"].get<std::string>();

    fmidi_smf_u midiFile { fmidi_smf_file_read(filepath.c_str()) };
    if (!midiFile) {
        return tl::make_unexpected(ReadingError::MidiFileError);
    }
    batteur::Sequence returned;

    const auto ib = json["ignore_bars"];
    if (!ib.is_null() && !ib.is_number_unsigned())
        return tl::make_unexpected(ReadingError::WrongIgnoreBars);
    const unsigned ignoreBars { ib.is_null() ? 0 : ib.get<unsigned>() };

    const auto b = json["bars"];
    if (!b.is_null()) {
        if (!b.is_number_unsigned())
            return tl::make_unexpected(ReadingError::WrongBars);
        if (b.get<unsigned>() == 0)
            return tl::make_unexpected(ReadingError::ZeroBars);
    }
    // Zero has a meaning internally
    const unsigned bars { b.is_null() ? 0 : b.get<unsigned>() };

    const auto updateIgnored = [ignoreBars] (unsigned qpb) -> double {
        return static_cast<double>(qpb * ignoreBars);
    };

    const auto updateEnd = [ignoreBars, bars] (unsigned qpb) -> double {
        if (bars > 0)
            return static_cast<double>(qpb *(ignoreBars + bars));
        else
            return 0.0;
    };

    const auto findLastNoteOn = [&returned](uint8_t number, double time) -> void {
        for (auto it = returned.rbegin(); it != returned.rend(); ++it) {
            if (it->number == number) {
                it->duration = max(0.0, time - it->timestamp);
                return;
            }
        }
    };

    fmidi_seq_u midiSequencer { fmidi_seq_new(midiFile.get()) };
    fmidi_seq_event_t event;
    unsigned quarterPerBars { 4 };
    double secondsPerQuarter { 0.5 };
    auto ignoredQuarters = updateIgnored(quarterPerBars);
    auto fileEnd = updateEnd(quarterPerBars);
    while (fmidi_seq_next_event(midiSequencer.get(), &event)) {
        const auto& evt = event.event;
        switch (evt->type) {
        case (fmidi_event_meta):
            if (auto qpb = getQuarterPerBars(*evt)) {
                quarterPerBars = *qpb;
                ignoredQuarters = updateIgnored(quarterPerBars);
                fileEnd = updateEnd(quarterPerBars);
            } else if (auto spq = getSecondsPerQuarter(*evt)) {
                secondsPerQuarter = *spq;
            }
            break;
        case (fmidi_event_message):
            // go to the next process step
            break;
        default:
            // Ignore other messages
            continue;
        }

        const double timeInQuarters = event.time / secondsPerQuarter;
        
        if (timeInQuarters < ignoredQuarters)
            continue;

        if (fileEnd > 0.0 && timeInQuarters > fileEnd)
            break;

        switch (midi::status(evt->data[0])) {
        case midi::noteOff:
            findLastNoteOn(evt->data[1], timeInQuarters);
            break;
        case midi::noteOn:
            // It's a note-off
            if (evt->data[2] == 0) {
                findLastNoteOn(evt->data[1], timeInQuarters);
                break;
            }

            // It's a real note-on
            returned.push_back({ timeInQuarters, 0.0, evt->data[1], evt->data[2] });
            break;
        default:
            break;
        }
    }

    if (returned.empty())
        return tl::make_unexpected(ReadingError::NoDataRead);

    for (auto& note : returned) {
        note.timestamp -= ignoredQuarters;
    }

#if 0
    DBG("Note NUM: TIME (DURATION)");
    for (auto& note : returned) {
        DBG("Note " << +note.number 
            << ": " << note.timestamp
            << "(" << note.duration << ")");
    }
#endif
    return returned;
}

tl::expected<batteur::Sequence, ReadingError> readSequenceFromNoteList(nlohmann::json& notes)
{
    if (!notes.is_array())
        return tl::make_unexpected(ReadingError::WrongNoteListFormat);

    batteur::Sequence returned;

    for (auto& note: notes) {
        const auto timeField = note["time"];
        if (timeField.is_null() || !timeField.is_number_float())
            return tl::make_unexpected(ReadingError::WrongTimeFormat);
        const auto time = timeField.get<double>();
        if (time < 0.0)
            return tl::make_unexpected(ReadingError::WrongTimeFormat);

        const auto durationField = note["duration"];
        if (durationField.is_null() || !durationField.is_number_float())
            return tl::make_unexpected(ReadingError::WrongNoteDuration);
        const auto duration = durationField.get<double>();
        if (duration < 0.0)
            return tl::make_unexpected(ReadingError::WrongNoteDuration);

        const auto numberField = note["number"];
        if (numberField.is_null() || !numberField.is_number_integer())
            return tl::make_unexpected(ReadingError::WrongNoteNumber);
        const auto number = numberField.get<uint8_t>();
        if (number > 127)
            return tl::make_unexpected(ReadingError::WrongNoteNumber);

        const auto valueField = note["velocity"];
        if (valueField.is_null() || !valueField.is_number_integer())
            return tl::make_unexpected(ReadingError::WrongNoteValue);
        const auto value = valueField.get<uint8_t>();
        if (value > 127)
            return tl::make_unexpected(ReadingError::WrongNoteValue);


        returned.push_back({ time, duration, number, value });
    }

    if (returned.empty())
        return tl::make_unexpected(ReadingError::NoDataRead);

    const auto timestampComparator = [](const batteur::Note& lhs, const batteur::Note& rhs) {
        return lhs.timestamp < rhs.timestamp;
    };
    std::sort(returned.begin(), returned.end(), timestampComparator);

    return returned;
}


// Note that the passed file should not be const
tl::expected<batteur::Sequence, ReadingError> readSequence(nlohmann::json& json, const fs::path& rootDirectory)
{
    if (json.is_null())
        return tl::make_unexpected(ReadingError::NotPresent);

    if (json.contains("filename"))
        return readSequenceFromFile(json, rootDirectory);
    else if (json.contains("notes"))
        return readSequenceFromNoteList(json["notes"]);
    else
        return tl::make_unexpected(ReadingError::NotPresent);    
}

tl::expected<double, BPMError> checkBPM(const nlohmann::json& bpm)
{
    if (bpm.is_null())
        return tl::make_unexpected(BPMError::NotPresent);

    if (!bpm.is_number())
        return tl::make_unexpected(BPMError::NotANumber);

    const auto b = bpm.get<double>();

    if (b <= 0.0)
        return tl::make_unexpected(BPMError::Negative);

    return b;
}

tl::expected<unsigned, QuartersPerBarError> checkQuartersPerBar(const nlohmann::json& qpb)
{
    if (qpb.is_null())
        return tl::make_unexpected(QuartersPerBarError::NotPresent);

    if (!qpb.is_number_unsigned())
        return tl::make_unexpected(QuartersPerBarError::NotAnUnsigned);

    const auto q = qpb.get<unsigned>();

    if (q == 0)
        return tl::make_unexpected(QuartersPerBarError::Zero);

    return q;
}