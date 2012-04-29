/*
Copyright 2012 Aphid Mobile

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
 
   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

 */

#include <config.h>
#include "TextCodecICU.h"

#include <unicode/ucnv.h>
#include <unicode/ucnv_cb.h>
#include <wtf/text/CString.h>

#include "OAUtil.h"

namespace Aphid {
	using Aphid::String;
	using ATF::CString;
	
	const size_t ConversionBufferSize = 16384;
	
	ICUConverterWrapper::~ICUConverterWrapper()
	{
    if (converter)
			ucnv_close(converter);
	}
	
	PassOwnPtr<TextCodec> newTextCodecICU(const TextEncoding& encoding, const void*)
	{
    return new TextCodecICU(encoding);
	}
	
	void TextCodecICU::registerBaseEncodingNames(EncodingNameRegistrar registrar)
	{
    registrar("UTF-8", "UTF-8");
	}
	
	void TextCodecICU::registerBaseCodecs(TextCodecRegistrar registrar)
	{
    registrar("UTF-8", newTextCodecICU, 0);
	}
		
	TextCodecICU::TextCodecICU(const TextEncoding& encoding)
	: m_encoding(encoding)
	, m_numBufferedBytes(0)
	, m_converterICU(0)
	, m_needsGBKFallbacks(false)
	{
	}
	
	TextCodecICU::~TextCodecICU()
	{
    releaseICUConverter();
	}
	
	void TextCodecICU::releaseICUConverter() const
	{
    if (m_converterICU) {
			ucnv_close(m_converterICU);
			m_converterICU = 0;
    }
	}
	
	void TextCodecICU::createICUConverter() const
	{
    ASSERT(!m_converterICU);
		
    const char* name = m_encoding.name();
    m_needsGBKFallbacks = name[0] == 'G' && name[1] == 'B' && name[2] == 'K' && !name[3];
		
    UErrorCode err;
		
    err = U_ZERO_ERROR;
    m_converterICU = ucnv_open(m_encoding.name(), &err);
#if OA_DEV
    if (err == U_AMBIGUOUS_ALIAS_WARNING)
			oa_error("ICU ambiguous alias warning for encoding: %s", m_encoding.name());
#endif
    if (m_converterICU)
			ucnv_setFallback(m_converterICU, TRUE);
	}
	
	int TextCodecICU::decodeToBuffer(UChar* target, UChar* targetLimit, const char*& source, const char* sourceLimit, int32_t* offsets, bool flush, UErrorCode& err)
	{
    UChar* targetStart = target;
    err = U_ZERO_ERROR;
    ucnv_toUnicode(m_converterICU, &target, targetLimit, &source, sourceLimit, offsets, flush, &err);
    return target - targetStart;
	}
	
	class ErrorCallbackSetter {
	public:
    ErrorCallbackSetter(UConverter* converter, bool stopOnError)
		: m_converter(converter)
		, m_shouldStopOnEncodingErrors(stopOnError)
    {
			if (m_shouldStopOnEncodingErrors) {
				UErrorCode err = U_ZERO_ERROR;
				ucnv_setToUCallBack(m_converter, UCNV_TO_U_CALLBACK_SUBSTITUTE,
														UCNV_SUB_STOP_ON_ILLEGAL, &m_savedAction,
														&m_savedContext, &err);
				ASSERT(err == U_ZERO_ERROR);
			}
    }
    ~ErrorCallbackSetter()
    {
			if (m_shouldStopOnEncodingErrors) {
				UErrorCode err = U_ZERO_ERROR;
				const void* oldContext;
				UConverterToUCallback oldAction;
				ucnv_setToUCallBack(m_converter, m_savedAction,
														m_savedContext, &oldAction,
														&oldContext, &err);
				ASSERT(oldAction == UCNV_TO_U_CALLBACK_SUBSTITUTE);
				ASSERT(!strcmp(static_cast<const char*>(oldContext), UCNV_SUB_STOP_ON_ILLEGAL));
				ASSERT(err == U_ZERO_ERROR);
			}
    }
	private:
    UConverter* m_converter;
    bool m_shouldStopOnEncodingErrors;
    const void* m_savedContext;
    UConverterToUCallback m_savedAction;
	};
	
	String TextCodecICU::decode(const char* bytes, size_t length, bool flush, bool stopOnError, bool& sawError)
	{
    // Get a converter for the passed-in encoding.
    if (!m_converterICU) {
			createICUConverter();
			ASSERT(m_converterICU);
			if (!m_converterICU) {
				LOG_ERROR("error creating ICU encoder even though encoding was in table");
				return String();
			}
    }
    
    ErrorCallbackSetter callbackSetter(m_converterICU, stopOnError);
		
    Vector<UChar> result;
		
    UChar buffer[ConversionBufferSize];
    UChar* bufferLimit = buffer + ConversionBufferSize;
    const char* source = reinterpret_cast<const char*>(bytes);
    const char* sourceLimit = source + length;
    int32_t* offsets = NULL;
    UErrorCode err = U_ZERO_ERROR;
		
    do {
			int ucharsDecoded = decodeToBuffer(buffer, bufferLimit, source, sourceLimit, offsets, flush, err);
			result.append(buffer, ucharsDecoded);
    } while (err == U_BUFFER_OVERFLOW_ERROR);
		
    if (U_FAILURE(err)) {
			// flush the converter so it can be reused, and not be bothered by this error.
			do {
				decodeToBuffer(buffer, bufferLimit, source, sourceLimit, offsets, true, err);
			} while (source < sourceLimit);
			sawError = true;
    }
		
    String resultString = String::adopt(result);
		
    // <http://bugs.webkit.org/show_bug.cgi?id=17014>
    // Simplified Chinese pages use the code A3A0 to mean "full-width space", but ICU decodes it as U+E5E5.
    if (strcmp(m_encoding.name(), "GBK") == 0 || strcasecmp(m_encoding.name(), "gb18030") == 0)
			resultString.replace(0xE5E5, ideographicSpace);
		
    return resultString;
	}
	
	// We need to apply these fallbacks ourselves as they are not currently supported by ICU and
	// they were provided by the old TEC encoding path
	// Needed to fix <rdar://problem/4708689>
	static UChar getGbkEscape(UChar32 codePoint)
	{
    switch (codePoint) {
			case 0x01F9:
				return 0xE7C8;
			case 0x1E3F:
				return 0xE7C7;
			case 0x22EF:
				return 0x2026;
			case 0x301C:
				return 0xFF5E;
			default:
				return 0;
    }
	}
	
	// Invalid character handler when writing escaped entities for unrepresentable
	// characters. See the declaration of TextCodec::encode for more.
	static void urlEscapedEntityCallback(const void* context, UConverterFromUnicodeArgs* fromUArgs, const UChar* codeUnits, int32_t length,
																			 UChar32 codePoint, UConverterCallbackReason reason, UErrorCode* err)
	{
    if (reason == UCNV_UNASSIGNED) {
			*err = U_ZERO_ERROR;
			
			UnencodableReplacementArray entity;
			int entityLen = TextCodec::getUnencodableReplacement(codePoint, URLEncodedEntitiesForUnencodables, entity);
			ucnv_cbFromUWriteBytes(fromUArgs, entity, entityLen, 0, err);
    } else
			UCNV_FROM_U_CALLBACK_ESCAPE(context, fromUArgs, codeUnits, length, codePoint, reason, err);
	}
	
	// Substitutes special GBK characters, escaping all other unassigned entities.
	static void gbkCallbackEscape(const void* context, UConverterFromUnicodeArgs* fromUArgs, const UChar* codeUnits, int32_t length,
																UChar32 codePoint, UConverterCallbackReason reason, UErrorCode* err) 
	{
    UChar outChar;
    if (reason == UCNV_UNASSIGNED && (outChar = getGbkEscape(codePoint))) {
			const UChar* source = &outChar;
			*err = U_ZERO_ERROR;
			ucnv_cbFromUWriteUChars(fromUArgs, &source, source + 1, 0, err);
			return;
    }
    UCNV_FROM_U_CALLBACK_ESCAPE(context, fromUArgs, codeUnits, length, codePoint, reason, err);
	}
	
	// Combines both gbkUrlEscapedEntityCallback and GBK character substitution.
	static void gbkUrlEscapedEntityCallack(const void* context, UConverterFromUnicodeArgs* fromUArgs, const UChar* codeUnits, int32_t length,
																				 UChar32 codePoint, UConverterCallbackReason reason, UErrorCode* err) 
	{
    if (reason == UCNV_UNASSIGNED) {
			if (UChar outChar = getGbkEscape(codePoint)) {
				const UChar* source = &outChar;
				*err = U_ZERO_ERROR;
				ucnv_cbFromUWriteUChars(fromUArgs, &source, source + 1, 0, err);
				return;
			}
			urlEscapedEntityCallback(context, fromUArgs, codeUnits, length, codePoint, reason, err);
			return;
    }
    UCNV_FROM_U_CALLBACK_ESCAPE(context, fromUArgs, codeUnits, length, codePoint, reason, err);
	}
	
	static void gbkCallbackSubstitute(const void* context, UConverterFromUnicodeArgs* fromUArgs, const UChar* codeUnits, int32_t length,
																		UChar32 codePoint, UConverterCallbackReason reason, UErrorCode* err) 
	{
    UChar outChar;
    if (reason == UCNV_UNASSIGNED && (outChar = getGbkEscape(codePoint))) {
			const UChar* source = &outChar;
			*err = U_ZERO_ERROR;
			ucnv_cbFromUWriteUChars(fromUArgs, &source, source + 1, 0, err);
			return;
    }
    UCNV_FROM_U_CALLBACK_SUBSTITUTE(context, fromUArgs, codeUnits, length, codePoint, reason, err);
	}
	
	CString TextCodecICU::encode(const UChar* characters, size_t length, UnencodableHandling handling)
	{
    if (!length)
			return "";
		
    if (!m_converterICU)
			createICUConverter();
    if (!m_converterICU)
			return CString();
		
    // FIXME: We should see if there is "force ASCII range" mode in ICU;
    // until then, we change the backslash into a yen sign.
    // Encoding will change the yen sign back into a backslash.
    String copy(characters, length);
    copy = m_encoding.displayString(copy.impl());
		
    const UChar* source = copy.characters();
    const UChar* sourceLimit = source + copy.length();
		
    UErrorCode err = U_ZERO_ERROR;
		
    switch (handling) {
			case QuestionMarksForUnencodables:
				ucnv_setSubstChars(m_converterICU, "?", 1, &err);
				ucnv_setFromUCallBack(m_converterICU, m_needsGBKFallbacks ? gbkCallbackSubstitute : UCNV_FROM_U_CALLBACK_SUBSTITUTE, 0, 0, 0, &err);
				break;
			case EntitiesForUnencodables:
				ucnv_setFromUCallBack(m_converterICU, m_needsGBKFallbacks ? gbkCallbackEscape : UCNV_FROM_U_CALLBACK_ESCAPE, UCNV_ESCAPE_XML_DEC, 0, 0, &err);
				break;
			case URLEncodedEntitiesForUnencodables:
				ucnv_setFromUCallBack(m_converterICU, m_needsGBKFallbacks ? gbkUrlEscapedEntityCallack : urlEscapedEntityCallback, 0, 0, 0, &err);
				break;
    }
		
    ASSERT(U_SUCCESS(err));
    if (U_FAILURE(err))
			return CString();
		
    Vector<char> result;
    size_t size = 0;
    do {
			char buffer[ConversionBufferSize];
			char* target = buffer;
			char* targetLimit = target + ConversionBufferSize;
			err = U_ZERO_ERROR;
			ucnv_fromUnicode(m_converterICU, &target, targetLimit, &source, sourceLimit, 0, true, &err);
			size_t count = target - buffer;
			result.grow(size + count);
			memcpy(result.data() + size, buffer, count);
			size += count;
    } while (err == U_BUFFER_OVERFLOW_ERROR);
		
    return CString(result.data(), size);
	}
}