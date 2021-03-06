// Copyright 2019 Google LLC.

#include "include/core/SkCanvas.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkTypeface.h"
#include "include/private/SkTFitsIn.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/Metrics.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/OneLineShaper.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/ParagraphUtil.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skparagraph/src/TextLine.h"
#include "modules/skparagraph/src/TextWrapper.h"
#include "src/core/SkSpan.h"
#include "src/utils/SkUTF.h"

#if defined(SK_USING_THIRD_PARTY_ICU)
#include "third_party/icu/SkLoadICU.h"
#endif

#include <math.h>
#include <unicode/ubidi.h>
#include <unicode/uloc.h>
#include <unicode/umachine.h>
#include <unicode/ustring.h>
#include <unicode/utext.h>
#include <unicode/utypes.h>
#include <algorithm>
#include <utility>


namespace skia {
namespace textlayout {

namespace {

using ICUUText = std::unique_ptr<UText, SkFunctionWrapper<decltype(utext_close), utext_close>>;
using ICUBiDi  = std::unique_ptr<UBiDi, SkFunctionWrapper<decltype(ubidi_close), ubidi_close>>;

SkScalar littleRound(SkScalar a) {
    // This rounding is done to match Flutter tests. Must be removed..
    auto val = std::fabs(a);
    if (val < 10000) {
        return SkScalarRoundToScalar(a * 100.0)/100.0;
    } else if (val < 100000) {
        return SkScalarRoundToScalar(a * 10.0)/10.0;
    } else {
        return SkScalarFloorToScalar(a);
    }
}

/** Replaces invalid utf-8 sequences with REPLACEMENT CHARACTER U+FFFD. */
static inline SkUnichar utf8_next(const char** ptr, const char* end) {
    SkUnichar val = SkUTF::NextUTF8(ptr, end);
    return val < 0 ? 0xFFFD : val;
}

}

TextRange operator*(const TextRange& a, const TextRange& b) {
    if (a.start == b.start && a.end == b.end) return a;
    auto begin = std::max(a.start, b.start);
    auto end = std::min(a.end, b.end);
    return end > begin ? TextRange(begin, end) : EMPTY_TEXT;
}

Paragraph::Paragraph(ParagraphStyle style, sk_sp<FontCollection> fonts)
            : fFontCollection(std::move(fonts))
            , fParagraphStyle(std::move(style))
            , fAlphabeticBaseline(0)
            , fIdeographicBaseline(0)
            , fHeight(0)
            , fWidth(0)
            , fMaxIntrinsicWidth(0)
            , fMinIntrinsicWidth(0)
            , fLongestLine(0)
            , fExceededMaxLines(0)
{ }

ParagraphImpl::ParagraphImpl(const SkString& text,
                             ParagraphStyle style,
                             SkTArray<Block, true> blocks,
                             SkTArray<Placeholder, true> placeholders,
                             sk_sp<FontCollection> fonts)
        : Paragraph(std::move(style), std::move(fonts))
        , fTextStyles(std::move(blocks))
        , fPlaceholders(std::move(placeholders))
        , fText(text)
        , fState(kUnknown)
        , fUnresolvedGlyphs(0)
        , fPicture(nullptr)
        , fStrutMetrics(false)
        , fOldWidth(0)
        , fOldHeight(0)
        , fOrigin(SkRect::MakeEmpty()) {
}

ParagraphImpl::ParagraphImpl(const std::u16string& utf16text,
                             ParagraphStyle style,
                             SkTArray<Block, true> blocks,
                             SkTArray<Placeholder, true> placeholders,
                             sk_sp<FontCollection> fonts)
        : ParagraphImpl(SkStringFromU16String(utf16text),
                        std::move(style),
                        std::move(blocks),
                        std::move(placeholders),
                        std::move(fonts))
{ }

ParagraphImpl::~ParagraphImpl() = default;

int32_t ParagraphImpl::unresolvedGlyphs() {
    if (fState < kShaped) {
        return -1;
    }

    return fUnresolvedGlyphs;
}

void ParagraphImpl::layout(SkScalar rawWidth) {

    // TODO: This rounding is done to match Flutter tests. Must be removed...
    auto floorWidth = SkScalarFloorToScalar(rawWidth);

    if ((!SkScalarIsFinite(rawWidth) || fLongestLine <= floorWidth) &&
        fState >= kLineBroken &&
         fLines.size() == 1 && fLines.front().ellipsis() == nullptr) {
        // Most common case: one line of text (and one line is never justified, so no cluster shifts)
        fWidth = floorWidth;
        fState = kLineBroken;
    } else if (fState >= kLineBroken && fOldWidth != floorWidth) {
        // We can use the results from SkShaper but have to do EVERYTHING ELSE again
        fState = kShaped;
    } else {
        // Nothing changed case: we can reuse the data from the last layout
    }

    if (fState < kShaped) {
        this->fCodeUnitProperties.reset();
        this->fCodeUnitProperties.push_back_n(fText.size() + 1, CodeUnitFlags::kNoCodeUnitFlag);
        this->fWords.clear();
        this->fBidiRegions.reset();
        this->fGraphemes16.reset();
        this->fCodepoints.reset();
        this->fRuns.reset();
        if (!this->shapeTextIntoEndlessLine()) {
            this->resetContext();
            // TODO: merge the two next calls - they always come together
            this->resolveStrut();
            this->computeEmptyMetrics();
            this->fLines.reset();

            // Set the important values that are not zero
            fWidth = floorWidth;
            fHeight = fEmptyMetrics.height();
            if (fParagraphStyle.getStrutStyle().getStrutEnabled() &&
                fParagraphStyle.getStrutStyle().getForceStrutHeight()) {
                fHeight = fStrutMetrics.height();
            }
            fAlphabeticBaseline = fEmptyMetrics.alphabeticBaseline();
            fIdeographicBaseline = fEmptyMetrics.ideographicBaseline();
            fLongestLine = FLT_MIN - FLT_MAX; // That is what flutter has
            fMinIntrinsicWidth = 0;
            fMaxIntrinsicWidth = 0;
            this->fOldWidth = floorWidth;
            this->fOldHeight = this->fHeight;

            return;
        }
        fState = kShaped;
    }

    if (fState < kMarked) {
        this->fClusters.reset();
        this->resetShifts();
        this->buildClusterTable();
        fState = kClusterized;
        this->spaceGlyphs();
        fState = kMarked;
    }

    if (fState < kLineBroken) {
        this->resetContext();
        this->resolveStrut();
        this->computeEmptyMetrics();
        this->fLines.reset();
        this->breakShapedTextIntoLines(floorWidth);
        fState = kLineBroken;
    }

    if (fState < kFormatted) {
        // Build the picture lazily not until we actually have to paint (or never)
        this->formatLines(fWidth);
        // We have to calculate the paragraph boundaries only after we format the lines
        this->calculateBoundaries();
        fState = kFormatted;
    }

    this->fOldWidth = floorWidth;
    this->fOldHeight = this->fHeight;

    // TODO: This rounding is done to match Flutter tests. Must be removed...
    fMinIntrinsicWidth = littleRound(fMinIntrinsicWidth);
    fMaxIntrinsicWidth = littleRound(fMaxIntrinsicWidth);

    // TODO: This is strictly Flutter thing. Must be factored out into some flutter code
    if (fParagraphStyle.getMaxLines() == 1 ||
        (fParagraphStyle.unlimited_lines() && fParagraphStyle.ellipsized())) {
        fMinIntrinsicWidth = fMaxIntrinsicWidth;
    }

    //SkDebugf("layout('%s', %f): %f %f\n", fText.c_str(), rawWidth, fMinIntrinsicWidth, fMaxIntrinsicWidth);
}

void ParagraphImpl::paint(SkCanvas* canvas, SkScalar x, SkScalar y) {

    if (fState < kDrawn) {
        // Record the picture anyway (but if we have some pieces in the cache they will be used)
        this->paintLinesIntoPicture();
        fState = kDrawn;
    }

    SkMatrix matrix = SkMatrix::Translate(x + fOrigin.fLeft, y + fOrigin.fTop);
    canvas->drawPicture(fPicture, &matrix, nullptr);
}

void ParagraphImpl::resetContext() {
    fAlphabeticBaseline = 0;
    fHeight = 0;
    fWidth = 0;
    fIdeographicBaseline = 0;
    fMaxIntrinsicWidth = 0;
    fMinIntrinsicWidth = 0;
    fLongestLine = 0;
    fMaxWidthWithTrailingSpaces = 0;
    fExceededMaxLines = false;
}

class TextBreaker {
public:
    TextBreaker() : fInitialized(false), fPos(-1) {}

    bool initialize(SkSpan<const char> text, UBreakIteratorType type) {

        UErrorCode status = U_ZERO_ERROR;
        fIterator = nullptr;
        fSize = text.size();
        UText sUtf8UText = UTEXT_INITIALIZER;
        std::unique_ptr<UText, SkFunctionWrapper<decltype(utext_close), utext_close>> utf8UText(
            utext_openUTF8(&sUtf8UText, text.begin(), text.size(), &status));
        if (U_FAILURE(status)) {
            SkDEBUGF("Could not create utf8UText: %s", u_errorName(status));
            return false;
        }
        fIterator.reset(ubrk_open(type, "en", nullptr, 0, &status));
        if (U_FAILURE(status)) {
            SkDEBUGF("Could not create line break iterator: %s", u_errorName(status));
            SK_ABORT("");
        }

        ubrk_setUText(fIterator.get(), utf8UText.get(), &status);
        if (U_FAILURE(status)) {
            SkDEBUGF("Could not setText on break iterator: %s", u_errorName(status));
            return false;
        }

        fInitialized = true;
        fPos = 0;
        return true;
    }

    bool initialized() const { return fInitialized; }

    size_t first() {
        fPos = ubrk_first(fIterator.get());
        return eof() ? fSize : fPos;
    }

    size_t next() {
        fPos = ubrk_next(fIterator.get());
        return eof() ? fSize : fPos;
    }

    size_t preceding(size_t offset) {
        auto pos = ubrk_preceding(fIterator.get(), offset);
        return pos == UBRK_DONE ? 0 : pos;
    }

    size_t following(size_t offset) {
        auto pos = ubrk_following(fIterator.get(), offset);
        return pos == UBRK_DONE ? fSize : pos;
    }

    int32_t status() { return ubrk_getRuleStatus(fIterator.get()); }

    bool eof() { return fPos == UBRK_DONE; }

private:
    std::unique_ptr<UBreakIterator, SkFunctionWrapper<decltype(ubrk_close), ubrk_close>> fIterator;
    bool fInitialized;
    int32_t fPos;
    size_t fSize;
};

// shapeTextIntoEndlessLine is the thing that calls this method
// (that contains all ICU dependencies except for words)
bool ParagraphImpl::computeCodeUnitProperties() {

    #if defined(SK_USING_THIRD_PARTY_ICU)
    if (!SkLoadICU()) {
        return false;
    }
    #endif

    {
        const char* start = fText.c_str();
        const char* end = start + fText.size();
        const char* ch = start;
        while (ch < end) {
            auto index = ch - start;
            auto unichar = utf8_next(&ch, end);
            if (u_isWhitespace(unichar)) {
                auto ending = ch - start;
                for (auto k = index; k < ending; ++k) {
                  fCodeUnitProperties[k] |= CodeUnitFlags::kPartOfWhiteSpace;
                }
            }
        }
    }
    {
        TextBreaker breaker;
        if (!breaker.initialize(this->text(), UBRK_LINE)) {
            return false;
        }
        while (!breaker.eof()) {
            size_t currentPos = breaker.next();
          fCodeUnitProperties[currentPos] |=
              breaker.status() == UBRK_LINE_HARD ? CodeUnitFlags::kHardLineBreakBefore : CodeUnitFlags::kSoftLineBreakBefore;
        }
    }
    {
        TextBreaker breaker;
        if (!breaker.initialize(this->text(), UBRK_CHARACTER)) {
            return false;
        }

        while (!breaker.eof()) {
            auto currentPos = breaker.next();
          fCodeUnitProperties[currentPos] |= CodeUnitFlags::kGraphemeBreakBefore;
        }
    }
/*
    SkString breaks;
    SkString graphemes;
    SkString whitespaces;
    size_t index = 0;
    for (auto flag : fIcuFlags) {
        if ((flag & IcuFlagTypes::kHardLineBreak) != 0) {
            breaks += "H";
        } else if ((flag & IcuFlagTypes::kSoftLineBreak) != 0) {
            breaks += "S";
        } else {
            breaks += " ";
        }
        graphemes += (flag & IcuFlagTypes::kGrapheme) == 0 ? " " : "G";
        whitespaces += (flag & IcuFlagTypes::kWhiteSpace) == 0 ? " " : "W";
        ++index;
    }
    SkDebugf("%s\n%s\n%s\n", breaks.c_str(), graphemes.c_str(), whitespaces.c_str());
*/
    return true;
}

// getWordBoundary is the thing that calls this method lazily
bool ParagraphImpl::computeWords() {

    if (!fWords.empty()) {
        return true;
    }

    UErrorCode errorCode = U_ZERO_ERROR;

    auto iter = ubrk_open(UBRK_WORD, uloc_getDefault(), nullptr, 0, &errorCode);
    if (U_FAILURE(errorCode)) {
        SkDEBUGF("Could not create line break iterator: %s", u_errorName(errorCode));
        return false;
    }

    // Getting the length like this seems to always set U_BUFFER_OVERFLOW_ERROR
    int32_t utf16Units;
    u_strFromUTF8(nullptr, 0, &utf16Units, fText.c_str(), fText.size(), &errorCode);
    errorCode = U_ZERO_ERROR;
    std::unique_ptr<UChar[]> utf16(new UChar[utf16Units]);
    u_strFromUTF8(utf16.get(), utf16Units, nullptr, fText.c_str(), fText.size(), &errorCode);
    if (U_FAILURE(errorCode)) {
        SkDEBUGF("Invalid utf8 input: %s", u_errorName(errorCode));
        return false;
    }

    UText sUtf16UText = UTEXT_INITIALIZER;
    ICUUText utf8UText(utext_openUChars(&sUtf16UText, utf16.get(), utf16Units, &errorCode));
    if (U_FAILURE(errorCode)) {
        SkDEBUGF("Could not create utf8UText: %s", u_errorName(errorCode));
        return false;
    }

    ubrk_setUText(iter, utf8UText.get(), &errorCode);
    if (U_FAILURE(errorCode)) {
        SkDEBUGF("Could not setText on break iterator: %s", u_errorName(errorCode));
        return false;
    }

    int32_t pos = ubrk_first(iter);
    while (pos != UBRK_DONE) {
        fWords.emplace_back(pos);
        pos = ubrk_next(iter);
    }

    return true;
}

bool ParagraphImpl::getBidiRegions() {

    if (!fBidiRegions.empty()) {
        return true;
    }

    // ubidi only accepts utf16 (though internally it basically works on utf32 chars).
    // We want an ubidi_setPara(UBiDi*, UText*, UBiDiLevel, UBiDiLevel*, UErrorCode*);
    size_t utf8Bytes = fText.size();
    const char* utf8 = fText.c_str();
    uint8_t bidiLevel = fParagraphStyle.getTextDirection() == TextDirection::kLtr
                            ? UBIDI_LTR
                            : UBIDI_RTL;
    if (!SkTFitsIn<int32_t>(utf8Bytes)) {
        SkDEBUGF("Bidi error: text too long");
        return false;
    }

    // Getting the length like this seems to always set U_BUFFER_OVERFLOW_ERROR
    UErrorCode status = U_ZERO_ERROR;
    int32_t utf16Units;
    u_strFromUTF8(nullptr, 0, &utf16Units, utf8, utf8Bytes, &status);
    status = U_ZERO_ERROR;
    std::unique_ptr<UChar[]> utf16(new UChar[utf16Units]);
    u_strFromUTF8(utf16.get(), utf16Units, nullptr, utf8, utf8Bytes, &status);
    if (U_FAILURE(status)) {
        SkDEBUGF("Invalid utf8 input: %s", u_errorName(status));
        return false;
    }

    ICUBiDi bidi(ubidi_openSized(utf16Units, 0, &status));
    if (U_FAILURE(status)) {
        SkDEBUGF("Bidi error: %s", u_errorName(status));
        return false;
    }
    SkASSERT(bidi);

    // The required lifetime of utf16 isn't well documented.
    // It appears it isn't used after ubidi_setPara except through ubidi_getText.
    ubidi_setPara(bidi.get(), utf16.get(), utf16Units, bidiLevel, nullptr, &status);
    if (U_FAILURE(status)) {
        SkDEBUGF("Bidi error: %s", u_errorName(status));
        return false;
    }

    SkTArray<BidiRegion> bidiRegions;
    const char* start8 = utf8;
    const char* end8 = utf8 + utf8Bytes;
    TextRange textRange(0, 0);
    UBiDiLevel currentLevel = 0;

    int32_t pos16 = 0;
    int32_t end16 = ubidi_getLength(bidi.get());
    while (pos16 < end16) {
        auto level = ubidi_getLevelAt(bidi.get(), pos16);
        if (pos16 == 0) {
            currentLevel = level;
        } else if (level != currentLevel) {
            textRange.end = start8 - utf8;
            fBidiRegions.emplace_back(textRange.start, textRange.end, currentLevel);
            currentLevel = level;
            textRange = TextRange(textRange.end, textRange.end);
        }
        SkUnichar u = utf8_next(&start8, end8);
        pos16 += SkUTF::ToUTF16(u);
    }

    textRange.end = start8 - utf8;
    if (!textRange.empty()) {
        fBidiRegions.emplace_back(textRange.start, textRange.end, currentLevel);
    }

    return true;
}

// Clusters in the order of the input text
void ParagraphImpl::buildClusterTable() {

    // Walk through all the run in the direction of input text
    for (auto& run : fRuns) {
        auto runIndex = run.index();
        auto runStart = fClusters.size();
        if (run.isPlaceholder()) {
            // There are no glyphs but we want to have one cluster
            fClusters.emplace_back(this, runIndex, 0ul, 1ul, this->text(run.textRange()), run.advance().fX, run.advance().fY);
            fCodeUnitProperties[run.textRange().start] |= CodeUnitFlags::kSoftLineBreakBefore;
            fCodeUnitProperties[run.textRange().end] |= CodeUnitFlags::kSoftLineBreakBefore;
        } else {
            fClusters.reserve(fClusters.size() + run.size());
            // Walk through the glyph in the direction of input text
            run.iterateThroughClustersInTextOrder([runIndex, this](size_t glyphStart,
                                                                   size_t glyphEnd,
                                                                   size_t charStart,
                                                                   size_t charEnd,
                                                                   SkScalar width,
                                                                   SkScalar height) {
                SkASSERT(charEnd >= charStart);
                SkSpan<const char> text(fText.c_str() + charStart, charEnd - charStart);
                fClusters.emplace_back(this, runIndex, glyphStart, glyphEnd, text, width, height);
            });
        }

        run.setClusterRange(runStart, fClusters.size());
        fMaxIntrinsicWidth += run.advance().fX;
    }
    fClusters.emplace_back(this, EMPTY_RUN, 0, 0, this->text({fText.size(), fText.size()}), 0, 0);
}

void ParagraphImpl::spaceGlyphs() {

    // Walk through all the clusters in the direction of shaped text
    // (we have to walk through the styles in the same order, too)
    SkScalar shift = 0;
    for (auto& run : fRuns) {

        // Skip placeholder runs
        if (run.isPlaceholder()) {
            continue;
        }

        bool soFarWhitespacesOnly = true;
        run.iterateThroughClusters([this, &run, &shift, &soFarWhitespacesOnly](Cluster* cluster) {
            // Shift the cluster (shift collected from the previous clusters)
            run.shift(cluster, shift);

            // Synchronize styles (one cluster can be covered by few styles)
            Block* currentStyle = this->fTextStyles.begin();
            while (!cluster->startsIn(currentStyle->fRange)) {
                currentStyle++;
                SkASSERT(currentStyle != this->fTextStyles.end());
            }

            SkASSERT(!currentStyle->fStyle.isPlaceholder());

            // Process word spacing
            if (currentStyle->fStyle.getWordSpacing() != 0) {
                if (cluster->isWhitespaces() && cluster->isSoftBreak()) {
                    if (!soFarWhitespacesOnly) {
                        shift += run.addSpacesAtTheEnd(currentStyle->fStyle.getWordSpacing(), cluster);
                    }
                }
            }
            // Process letter spacing
            if (currentStyle->fStyle.getLetterSpacing() != 0) {
                shift += run.addSpacesEvenly(currentStyle->fStyle.getLetterSpacing(), cluster);
            }

            if (soFarWhitespacesOnly && !cluster->isWhitespaces()) {
                soFarWhitespacesOnly = false;
            }
        });
    }
}

bool ParagraphImpl::shapeTextIntoEndlessLine() {

    if (fText.size() == 0) {
        return false;
    }

    // Check the font-resolved text against the cache
    if (fFontCollection->getParagraphCache()->findParagraph(this)) {
        return true;
    }

    if (!computeCodeUnitProperties()) {
        return false;
    }

    fFontSwitches.reset();

    OneLineShaper oneLineShaper(this);
    auto result = oneLineShaper.shape();
    fUnresolvedGlyphs = oneLineShaper.unresolvedGlyphs();

    if (!result) {
        return false;
    } else {
        // Add the paragraph to the cache
        fFontCollection->getParagraphCache()->updateParagraph(this);
        return true;
    }
}

void ParagraphImpl::breakShapedTextIntoLines(SkScalar maxWidth) {
    TextWrapper textWrapper;
    textWrapper.breakTextIntoLines(
            this,
            maxWidth,
            [&](TextRange text,
                TextRange textWithSpaces,
                ClusterRange clusters,
                ClusterRange clustersWithGhosts,
                SkScalar widthWithSpaces,
                size_t startPos,
                size_t endPos,
                SkVector offset,
                SkVector advance,
                InternalLineMetrics metrics,
                bool addEllipsis) {
                // TODO: Take in account clipped edges
                auto& line = this->addLine(offset, advance, text, textWithSpaces, clusters, clustersWithGhosts, widthWithSpaces, metrics);
                if (addEllipsis) {
                    line.createEllipsis(maxWidth, fParagraphStyle.getEllipsis(), true);
                    if (line.ellipsis() != nullptr) {
                        // Make sure the paragraph boundaries include its ellipsis
                        auto size = line.ellipsis()->advance();
                        auto offset = line.ellipsis()->offset();
                        SkRect boundaries = SkRect::MakeXYWH(offset.fX, offset.fY, size.fX, size.fY);
                        fOrigin.joinPossiblyEmptyRect(boundaries);
                    }
                }

                fLongestLine = std::max(fLongestLine, nearlyZero(advance.fX) ? widthWithSpaces : advance.fX);
            });

    fHeight = textWrapper.height();
    fWidth = maxWidth;
    fMaxIntrinsicWidth = textWrapper.maxIntrinsicWidth();
    fMinIntrinsicWidth = textWrapper.minIntrinsicWidth();
    fAlphabeticBaseline = fLines.empty() ? fEmptyMetrics.alphabeticBaseline() : fLines.front().alphabeticBaseline();
    fIdeographicBaseline = fLines.empty() ? fEmptyMetrics.ideographicBaseline() : fLines.front().ideographicBaseline();
    fExceededMaxLines = textWrapper.exceededMaxLines();

    // Correct the first and the last line ascents/descents if required
    if ((fParagraphStyle.getTextHeightBehavior() & TextHeightBehavior::kDisableFirstAscent) != 0) {
        auto& firstLine = fLines.front();
        auto delta = firstLine.metricsWithoutMultiplier(TextHeightBehavior::kDisableFirstAscent);
        if (!SkScalarNearlyZero(delta)) {
            fHeight += delta;
            // Shift all the lines up
            for (auto& line : fLines) {
                if (line.isFirstLine()) continue;
                line.shiftVertically(delta);
            }
        }
    }

    if ((fParagraphStyle.getTextHeightBehavior() & TextHeightBehavior::kDisableLastDescent) != 0) {
        auto& lastLine = fLines.back();
        auto delta = lastLine.metricsWithoutMultiplier(TextHeightBehavior::kDisableLastDescent);
        // It's the last line. There is nothing below to shift
        fHeight += delta;
    }
}

void ParagraphImpl::formatLines(SkScalar maxWidth) {
    auto effectiveAlign = fParagraphStyle.effective_align();

    if (!SkScalarIsFinite(maxWidth) && effectiveAlign != TextAlign::kLeft) {
        // Special case: clean all text in case of maxWidth == INF & align != left
        // We had to go through shaping though because we need all the measurement numbers
        fLines.reset();
        return;
    }

    for (auto& line : fLines) {
        line.format(effectiveAlign, maxWidth);
    }
}

void ParagraphImpl::paintLinesIntoPicture() {
    SkPictureRecorder recorder;
    SkCanvas* textCanvas = recorder.beginRecording(fOrigin.width(), fOrigin.height(), nullptr, 0);
    textCanvas->translate(-fOrigin.fLeft, -fOrigin.fTop);

    for (auto& line : fLines) {
        line.paint(textCanvas);
    }

    fPicture = recorder.finishRecordingAsPicture();
}

void ParagraphImpl::resolveStrut() {
    auto strutStyle = this->paragraphStyle().getStrutStyle();
    if (!strutStyle.getStrutEnabled() || strutStyle.getFontSize() < 0) {
        return;
    }

    std::vector<sk_sp<SkTypeface>> typefaces = fFontCollection->findTypefaces(strutStyle.getFontFamilies(), strutStyle.getFontStyle());
    if (typefaces.empty()) {
        SkDEBUGF("Could not resolve strut font\n");
        return;
    }

    SkFont font(typefaces.front(), strutStyle.getFontSize());
    SkFontMetrics metrics;
    font.getMetrics(&metrics);

    if (strutStyle.getHeightOverride()) {
        auto strutHeight = metrics.fDescent - metrics.fAscent;
        auto strutMultiplier = strutStyle.getHeight() * strutStyle.getFontSize();
        fStrutMetrics = InternalLineMetrics(
            (metrics.fAscent / strutHeight) * strutMultiplier,
            (metrics.fDescent / strutHeight) * strutMultiplier,
                strutStyle.getLeading() < 0 ? 0 : strutStyle.getLeading() * strutStyle.getFontSize());
    } else {
        fStrutMetrics = InternalLineMetrics(
                metrics.fAscent,
                metrics.fDescent,
                strutStyle.getLeading() < 0 ? 0
                                            : strutStyle.getLeading() * strutStyle.getFontSize());
    }
    fStrutMetrics.setForceStrut(this->paragraphStyle().getStrutStyle().getForceStrutHeight());
}

BlockRange ParagraphImpl::findAllBlocks(TextRange textRange) {
    BlockIndex begin = EMPTY_BLOCK;
    BlockIndex end = EMPTY_BLOCK;
    for (size_t index = 0; index < fTextStyles.size(); ++index) {
        auto& block = fTextStyles[index];
        if (block.fRange.end <= textRange.start) {
            continue;
        }
        if (block.fRange.start >= textRange.end) {
            break;
        }
        if (begin == EMPTY_BLOCK) {
            begin = index;
        }
        end = index;
    }

    return { begin, end + 1 };
}

void ParagraphImpl::calculateBoundaries() {
    for (auto& line : fLines) {
        fOrigin.joinPossiblyEmptyRect(line.calculateBoundaries());
    }
}

TextLine& ParagraphImpl::addLine(SkVector offset,
                                 SkVector advance,
                                 TextRange text,
                                 TextRange textWithSpaces,
                                 ClusterRange clusters,
                                 ClusterRange clustersWithGhosts,
                                 SkScalar widthWithSpaces,
                                 InternalLineMetrics sizes) {
    // Define a list of styles that covers the line
    auto blocks = findAllBlocks(text);
    return fLines.emplace_back(this, offset, advance, blocks, text, textWithSpaces, clusters, clustersWithGhosts, widthWithSpaces, sizes);
}

void ParagraphImpl::markGraphemes16() {

    if (!fGraphemes16.empty()) {
        return;
    }

    // Fill out code points 16
    auto ptr = fText.c_str();
    auto end = fText.c_str() + fText.size();
    while (ptr < end) {

        size_t index = ptr - fText.c_str();
        SkUnichar u = SkUTF::NextUTF8(&ptr, end);
        uint16_t buffer[2];
        size_t count = SkUTF::ToUTF16(u, buffer);
        fCodepoints.emplace_back(EMPTY_INDEX, index, count > 1 ? 2 : 1);
        if (count > 1) {
            fCodepoints.emplace_back(EMPTY_INDEX, index, 1);
        }
    }

    CodepointRange codepoints(0ul, 0ul);

  forEachCodeUnitPropertyRange(
      CodeUnitFlags::kGraphemeBreakBefore,
      [&](TextRange textRange) {
        // Collect all the codepoints that belong to the grapheme
        while (codepoints.end < fCodepoints.size()
            && fCodepoints[codepoints.end].fTextIndex < textRange.end) {
          ++codepoints.end;
        }

        if (textRange.start == textRange.end) {
          return true;
        }

        //SkDebugf("Grapheme #%d [%d:%d)\n", fGraphemes16.size(), startPos, endPos);

        // Update all the codepoints that belong to this grapheme
        for (auto i = codepoints.start; i < codepoints.end; ++i) {
          //SkDebugf("   [%d] = %d + %d\n", i, fCodePoints[i].fTextIndex, fCodePoints[i].fIndex);
          fCodepoints[i].fGrapheme = fGraphemes16.size();
        }

        fGraphemes16.emplace_back(codepoints, textRange);
        codepoints.start = codepoints.end;
        return true;
      });
}

// Returns a vector of bounding boxes that enclose all text between
// start and end glyph indexes, including start and excluding end
std::vector<TextBox> ParagraphImpl::getRectsForRange(unsigned start,
                                                     unsigned end,
                                                     RectHeightStyle rectHeightStyle,
                                                     RectWidthStyle rectWidthStyle) {
    std::vector<TextBox> results;
    if (fText.isEmpty()) {
        if (start == 0 && end > 0) {
            // On account of implied "\n" that is always at the end of the text
            //SkDebugf("getRectsForRange(%d, %d): %f\n", start, end, fHeight);
            results.emplace_back(SkRect::MakeXYWH(0, 0, 0, fHeight), fParagraphStyle.getTextDirection());
        }
        return results;
    }

    markGraphemes16();

    if (start >= end || start > fCodepoints.size() || end == 0) {
        return results;
    }

    // Adjust the text to grapheme edges
    // Apparently, text editor CAN move inside graphemes but CANNOT select a part of it.
    // I don't know why - the solution I have here returns an empty box for every query that
    // does not contain an end of a grapheme.
    // Once a cursor is inside a complex grapheme I can press backspace and cause trouble.
    // To avoid any problems, I will not allow any selection of a part of a grapheme.
    // One flutter test fails because of it but the editing experience is correct
    // (although you have to press the cursor many times before it moves to the next grapheme).
    TextRange text(fText.size(), fText.size());
    if (start < fCodepoints.size()) {
        auto codepoint = fCodepoints[start];
        auto grapheme = fGraphemes16[codepoint.fGrapheme];
        text.start = grapheme.fTextRange.start;
    }

    if (end < fCodepoints.size()) {
        auto codepoint = fCodepoints[end];
        auto grapheme = fGraphemes16[codepoint.fGrapheme];
        text.end = grapheme.fTextRange.start;
    }

    for (auto& line : fLines) {
        auto lineText = line.textWithSpaces();
        auto intersect = lineText * text;
        if (intersect.empty() && lineText.start != text.start) {
            continue;
        }

        line.getRectsForRange(intersect, rectHeightStyle, rectWidthStyle, results);
    }
/*
    SkDebugf("getRectsForRange(%d, %d)\n", start, end);
    for (auto& r : results) {
        r.rect.fLeft = littleRound(r.rect.fLeft);
        r.rect.fRight = littleRound(r.rect.fRight);
        r.rect.fTop = littleRound(r.rect.fTop);
        r.rect.fBottom = littleRound(r.rect.fBottom);
        SkDebugf("[%f:%f * %f:%f]\n", r.rect.fLeft, r.rect.fRight, r.rect.fTop, r.rect.fBottom);
    }
*/
    return results;
}

std::vector<TextBox> ParagraphImpl::getRectsForPlaceholders() {
  std::vector<TextBox> boxes;
  if (fText.isEmpty()) {
       return boxes;
  }
  if (fPlaceholders.size() == 1) {
       // We always have one fake placeholder
       return boxes;
  }
  for (auto& line : fLines) {
      line.getRectsForPlaceholders(boxes);
  }
  /*
  SkDebugf("getRectsForPlaceholders('%s'): %d\n", fText.c_str(), boxes.size());
  for (auto& r : boxes) {
      r.rect.fLeft = littleRound(r.rect.fLeft);
      r.rect.fRight = littleRound(r.rect.fRight);
      r.rect.fTop = littleRound(r.rect.fTop);
      r.rect.fBottom = littleRound(r.rect.fBottom);
      SkDebugf("[%f:%f * %f:%f] %s\n", r.rect.fLeft, r.rect.fRight, r.rect.fTop, r.rect.fBottom,
               (r.direction == TextDirection::kLtr ? "left" : "right"));
  }
  */
  return boxes;
}

// TODO: Optimize (save cluster <-> codepoint connection)
PositionWithAffinity ParagraphImpl::getGlyphPositionAtCoordinate(SkScalar dx, SkScalar dy) {

    if (fText.isEmpty()) {
        return {0, Affinity::kDownstream};
    }

    markGraphemes16();
    for (auto& line : fLines) {
        // Let's figure out if we can stop looking
        auto offsetY = line.offset().fY;
        if (dy >= offsetY + line.height() && &line != &fLines.back()) {
            // This line is not good enough
            continue;
        }

        // This is so far the the line vertically closest to our coordinates
        // (or the first one, or the only one - all the same)

        auto result = line.getGlyphPositionAtCoordinate(dx);
        //SkDebugf("getGlyphPositionAtCoordinate(%f, %f): %d %s\n", dx, dy, result.position,
        //   result.affinity == Affinity::kUpstream ? "up" : "down");
        return result;
    }

    return {0, Affinity::kDownstream};
}

// Finds the first and last glyphs that define a word containing
// the glyph at index offset.
// By "glyph" they mean a character index - indicated by Minikin's code
SkRange<size_t> ParagraphImpl::getWordBoundary(unsigned offset) {

    if (!computeWords()) {
        return {0, 0 };
    }

    int32_t start = 0;
    int32_t end = 0;
    for (size_t i = 0; i < fWords.size(); ++i) {
      auto word = fWords[i];
      if (word <= offset) {
        start = word;
        end = word;
      } else if (word > offset) {
        end = word;
        break;
      }
    }

    //SkDebugf("getWordBoundary(%d): %d - %d\n", offset, start, end);
    return { SkToU32(start), SkToU32(end) };
}

void ParagraphImpl::forEachCodeUnitPropertyRange(CodeUnitFlags property, CodeUnitRangeVisitor visitor) {

    size_t first = 0;
    for (size_t i = 1; i < fText.size(); ++i) {
        auto properties = fCodeUnitProperties[i];
        if (properties & property) {
            visitor({first, i});
            first = i;
        }

    }
    visitor({first, fText.size()});
}

size_t ParagraphImpl::getWhitespacesLength(TextRange textRange) {
    size_t len = 0;
    for (auto i = textRange.start; i < textRange.end; ++i) {
        auto properties = fCodeUnitProperties[i];
        if (properties & CodeUnitFlags::kPartOfWhiteSpace) {
            ++len;
        }
    }
    return len;
}

void ParagraphImpl::getLineMetrics(std::vector<LineMetrics>& metrics) {
    metrics.clear();
    for (auto& line : fLines) {
        metrics.emplace_back(line.getMetrics());
    }
}

SkSpan<const char> ParagraphImpl::text(TextRange textRange) {
    SkASSERT(textRange.start <= fText.size() && textRange.end <= fText.size());
    auto start = fText.c_str() + textRange.start;
    return SkSpan<const char>(start, textRange.width());
}

SkSpan<Cluster> ParagraphImpl::clusters(ClusterRange clusterRange) {
    SkASSERT(clusterRange.start < fClusters.size() && clusterRange.end <= fClusters.size());
    return SkSpan<Cluster>(&fClusters[clusterRange.start], clusterRange.width());
}

Cluster& ParagraphImpl::cluster(ClusterIndex clusterIndex) {
    SkASSERT(clusterIndex < fClusters.size());
    return fClusters[clusterIndex];
}

Run& ParagraphImpl::run(RunIndex runIndex) {
    SkASSERT(runIndex < fRuns.size());
    return fRuns[runIndex];
}

Run& ParagraphImpl::runByCluster(ClusterIndex clusterIndex) {
    auto start = cluster(clusterIndex);
    return this->run(start.fRunIndex);
}

SkSpan<Block> ParagraphImpl::blocks(BlockRange blockRange) {
    SkASSERT(blockRange.start < fTextStyles.size() && blockRange.end <= fTextStyles.size());
    return SkSpan<Block>(&fTextStyles[blockRange.start], blockRange.width());
}

Block& ParagraphImpl::block(BlockIndex blockIndex) {
    SkASSERT(blockIndex < fTextStyles.size());
    return fTextStyles[blockIndex];
}

void ParagraphImpl::setState(InternalState state) {
    if (fState <= state) {
        fState = state;
        return;
    }

    fState = state;
    switch (fState) {
        case kUnknown:
            fRuns.reset();
            fCodeUnitProperties.reset();
            fCodeUnitProperties.push_back_n(fText.size() + 1, kNoCodeUnitFlag);
            fWords.clear();
            fBidiRegions.reset();
            fGraphemes16.reset();
            fCodepoints.reset();
        case kShaped:
            fClusters.reset();
        case kClusterized:
        case kMarked:
        case kLineBroken:
            this->resetContext();
            this->resolveStrut();
            this->computeEmptyMetrics();
            this->resetShifts();
            fLines.reset();
        case kFormatted:
            fPicture = nullptr;
        case kDrawn:
            break;
    default:
        break;
    }
}

void ParagraphImpl::computeEmptyMetrics() {
    auto defaultTextStyle = paragraphStyle().getTextStyle();

    auto typefaces = fontCollection()->findTypefaces(
      defaultTextStyle.getFontFamilies(), defaultTextStyle.getFontStyle());
    auto typeface = typefaces.empty() ? nullptr : typefaces.front();

    SkFont font(typeface, defaultTextStyle.getFontSize());

    fEmptyMetrics = InternalLineMetrics(font, paragraphStyle().getStrutStyle().getForceStrutHeight());
    if (!paragraphStyle().getStrutStyle().getForceStrutHeight() &&
        defaultTextStyle.getHeightOverride()) {
        auto multiplier =
                defaultTextStyle.getHeight() * defaultTextStyle.getFontSize() / fEmptyMetrics.height();
        fEmptyMetrics = InternalLineMetrics(fEmptyMetrics.ascent() * multiplier,
                                      fEmptyMetrics.descent() * multiplier,
                                      fEmptyMetrics.leading() * multiplier);
    }

    if (fParagraphStyle.getStrutStyle().getStrutEnabled()) {
        fStrutMetrics.updateLineMetrics(fEmptyMetrics);
    }
}

void ParagraphImpl::updateText(size_t from, SkString text) {
  fText.remove(from, from + text.size());
  fText.insert(from, text);
  fState = kUnknown;
  fOldWidth = 0;
  fOldHeight = 0;
}

void ParagraphImpl::updateFontSize(size_t from, size_t to, SkScalar fontSize) {

  SkASSERT(from == 0 && to == fText.size());
  auto defaultStyle = fParagraphStyle.getTextStyle();
  defaultStyle.setFontSize(fontSize);
  fParagraphStyle.setTextStyle(defaultStyle);

  for (auto& textStyle : fTextStyles) {
    textStyle.fStyle.setFontSize(fontSize);
  }

  fState = kUnknown;
  fOldWidth = 0;
  fOldHeight = 0;
}

void ParagraphImpl::updateTextAlign(TextAlign textAlign) {
    fParagraphStyle.setTextAlign(textAlign);

    if (fState >= kLineBroken) {
        fState = kLineBroken;
    }
}

void ParagraphImpl::updateForegroundPaint(size_t from, size_t to, SkPaint paint) {
    SkASSERT(from == 0 && to == fText.size());
    auto defaultStyle = fParagraphStyle.getTextStyle();
    defaultStyle.setForegroundColor(paint);
    fParagraphStyle.setTextStyle(defaultStyle);

    for (auto& textStyle : fTextStyles) {
        textStyle.fStyle.setForegroundColor(paint);
    }
}

void ParagraphImpl::updateBackgroundPaint(size_t from, size_t to, SkPaint paint) {
    SkASSERT(from == 0 && to == fText.size());
    auto defaultStyle = fParagraphStyle.getTextStyle();
    defaultStyle.setBackgroundColor(paint);
    fParagraphStyle.setTextStyle(defaultStyle);

    for (auto& textStyle : fTextStyles) {
        textStyle.fStyle.setBackgroundColor(paint);
    }
}

}  // namespace textlayout
}  // namespace skia
