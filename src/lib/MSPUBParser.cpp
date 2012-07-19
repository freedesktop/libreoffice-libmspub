/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* libmspub
 * Version: MPL 1.1 / GPLv2+ / LGPLv2+
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License or as specified alternatively below. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * Major Contributor(s):
 * Copyright (C) 2012 Brennan Vincent <brennanv@email.arizona.edu>
 * Copyright (C) 2012 Fridrich Strba <fridrich.strba@bluewin.ch>
 *
 * All Rights Reserved.
 *
 * For minor contributions see the git repository.
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPLv2+"), or
 * the GNU Lesser General Public License Version 2 or later (the "LGPLv2+"),
 * in which case the provisions of the GPLv2+ or the LGPLv2+ are applicable
 * instead of those above.
 */

#include <sstream>
#include <string>
#include <algorithm>
#include <string.h>
#include <libwpd-stream/libwpd-stream.h>
#include <zlib.h>
#include "MSPUBParser.h"
#include "MSPUBCollector.h"
#include "MSPUBBlockID.h"
#include "MSPUBBlockType.h"
#include "MSPUBContentChunkType.h"
#include "MSPUBConstants.h"
#include "EscherContainerType.h"
#include "EscherFieldIds.h"
#include "libmspub_utils.h"
#include "ShapeType.h"
#include "ShapeFlags.h"
#include "Fill.h"
#include "FillType.h"

libmspub::MSPUBParser::MSPUBParser(WPXInputStream *input, MSPUBCollector *collector)
  : m_input(input), m_collector(collector),
    m_blockInfo(), m_contentChunks(),
    m_pageChunkIndices(), m_shapeChunkIndices(),
    m_paletteChunkIndices(), m_unknownChunkIndices(),
    m_documentChunkIndex(), m_lastSeenSeqNum(-1),
    m_lastAddedImage(0),
    m_alternateShapeSeqNums(), m_escherDelayIndices()
{
}

libmspub::MSPUBParser::~MSPUBParser()
{
}

bool libmspub::MSPUBParser::lineExistsByFlagPointer(unsigned *flags)
{
  return flags && !(((*flags) & FLAG_USE_LINE) && !((*flags) & FLAG_LINE));
}

unsigned libmspub::MSPUBParser::getColorIndexByQuillEntry(unsigned entry)
{
  return entry;
}

short libmspub::MSPUBParser::getBlockDataLength(unsigned type) // -1 for variable-length block with the data length as the first DWORD
{
  switch(type)
  {
  case DUMMY:
  case 0x5:
  case 0x8:
  case 0xa:
    return 0;
  case 0x10:
  case 0x12:
  case 0x18:
  case 0x1a:
  case 0x07:
    return 2;
  case 0x20:
  case 0x22:
  case 0x58:
  case 0x68:
  case 0x70:
  case 0xb8:
    return 4;
  case 0x28:
    return 8;
  case 0x38:
    return 16;
  case 0x48:
    return 24;
  case STRING_CONTAINER:
  case 0x80:
  case 0x82:
  case GENERAL_CONTAINER:
  case 0x8a:
  case 0x90:
  case 0x98:
  case 0xa0:
    return -1;
  }
  //FIXME: Debug assertion here? Should never get here.
  MSPUB_DEBUG_MSG(("Block of unknown type seen!\n"));
  return 0;
}

bool libmspub::MSPUBParser::parse()
{
  MSPUB_DEBUG_MSG(("***NOTE***: Where applicable, the meanings of block/chunk IDs and Types printed below may be found in:\n\t***MSPUBBlockType.h\n\t***MSPUBBlockID.h\n\t***MSPUBContentChunkType.h\n*****\n"));
  if (!m_input->isOLEStream())
    return false;
  WPXInputStream *quill = m_input->getDocumentOLEStream("Quill/QuillSub/CONTENTS");
  if (!quill)
  {
    MSPUB_DEBUG_MSG(("Couldn't get quill stream.\n"));
    return false;
  }
  if (!parseQuill(quill))
  {
    MSPUB_DEBUG_MSG(("Couldn't parse quill stream.\n"));
    delete quill;
    return false;
  }
  delete quill;
  WPXInputStream *contents = m_input->getDocumentOLEStream("Contents");
  if (!contents)
  {
    MSPUB_DEBUG_MSG(("Couldn't get contents stream.\n"));
    return false;
  }
  if (!parseContents(contents))
  {
    MSPUB_DEBUG_MSG(("Couldn't parse contents stream.\n"));
    delete contents;
    return false;
  }
  delete contents;
  WPXInputStream *escherDelay = m_input->getDocumentOLEStream("Escher/EscherDelayStm");
  if (escherDelay)
  {
    parseEscherDelay(escherDelay);
    delete escherDelay;
  }
  WPXInputStream *escher = m_input->getDocumentOLEStream("Escher/EscherStm");
  if (!escher)
  {
    MSPUB_DEBUG_MSG(("Couldn't get escher stream.\n"));
    return false;
  }
  if (!parseEscher(escher))
  {
    MSPUB_DEBUG_MSG(("Couldn't parse escher stream.\n"));
    delete escher;
    return false;
  }
  delete escher;

  return m_collector->go();
}

libmspub::ImgType libmspub::MSPUBParser::imgTypeByBlipType(unsigned short type)
{
  switch (type)
  {
  case OFFICE_ART_BLIP_PNG:
    return PNG;
  case OFFICE_ART_BLIP_JPEG:
    return JPEG;
  case OFFICE_ART_BLIP_WMF:
    return WMF;
  case OFFICE_ART_BLIP_DIB:
    return DIB;
  case OFFICE_ART_BLIP_EMF:
    return EMF;
  case OFFICE_ART_BLIP_TIFF:
    return TIFF;
  case OFFICE_ART_BLIP_PICT:
    return PICT;
  }
  return UNKNOWN;
}

int libmspub::MSPUBParser::getStartOffset(ImgType type, unsigned short initial)
{
  bool oneUid = true;
  int offset = 0x11;
  unsigned short recInstance = initial >> 4;
  switch (type)
  {
  case WMF:
    oneUid = recInstance == 0x216;
    offset = 0x34;
    break;
  case EMF:
    oneUid = recInstance == 0x3D4;
    offset = 0x34;
    break;
  case PNG:
    oneUid = recInstance == 0x6E0;
    offset = 0x11;
    break;
  case JPEG:
    oneUid = recInstance == 0x46A || recInstance == 0x6E2;
    offset = 0x11;
    break;
  case DIB:
    oneUid = recInstance == 0x7A8;
    offset = 0x11;
    break;
  default:
    break;
  }
  return offset + (oneUid ? 0 : 0x10);
}

bool libmspub::MSPUBParser::parseEscherDelay(WPXInputStream *input)
{
  while (stillReading (input, (unsigned long)-1))
  {
    EscherContainerInfo info = parseEscherContainer(input);
    if (imgTypeByBlipType(info.type) != UNKNOWN)
    {
      WPXBinaryData img;
      unsigned long toRead = info.contentsLength;
      input->seek(input->tell() + getStartOffset(imgTypeByBlipType(info.type), info.initial), WPX_SEEK_SET);
      while (toRead > 0 && stillReading(input, (unsigned long)-1))
      {
        unsigned long howManyRead = 0;
        const unsigned char *buf = input->read(toRead, howManyRead);
        img.append(buf, howManyRead);
        toRead -= howManyRead;
      }
      if (imgTypeByBlipType(info.type) == WMF || imgTypeByBlipType(info.type) == EMF)
      {
        img = inflateData(img);
      }
      else if (imgTypeByBlipType(info.type) == DIB)
      {
        // Reconstruct BMP header
        // cf. http://en.wikipedia.org/wiki/BMP_file_format , accessed 2012-5-31
        const unsigned char *buf = img.getDataBuffer();
        if (img.size() < 0x2E + 4)
        {
          ++m_lastAddedImage;
          MSPUB_DEBUG_MSG(("Garbage DIB at index 0x%x\n", m_lastAddedImage));
          input->seek(info.contentsOffset + info.contentsLength, WPX_SEEK_SET);
          continue;
        }
        unsigned short bitsPerPixel = readU16(buf, 0x0E);
        unsigned numPaletteColors = readU32(buf, 0x20);
        if (numPaletteColors == 0 && bitsPerPixel <= 8)
        {
          numPaletteColors = 1;
          for (int i = 0; i < bitsPerPixel; ++i)
          {
            numPaletteColors *= 2;
          }
        }

        WPXBinaryData tmpImg;
        tmpImg.append(0x42);
        tmpImg.append(0x4d);

        tmpImg.append((unsigned char)((img.size() + 14) & 0x000000ff));
        tmpImg.append((unsigned char)(((img.size() + 14) & 0x0000ff00) >> 8));
        tmpImg.append((unsigned char)(((img.size() + 14) & 0x00ff0000) >> 16));
        tmpImg.append((unsigned char)(((img.size() + 14) & 0xff000000) >> 24));

        tmpImg.append(0x00);
        tmpImg.append(0x00);
        tmpImg.append(0x00);
        tmpImg.append(0x00);

        tmpImg.append(0x36 + 4 * numPaletteColors);
        tmpImg.append(0x00);
        tmpImg.append(0x00);
        tmpImg.append(0x00);
        tmpImg.append(img);
        img = tmpImg;
      }
      m_collector->addImage(++m_lastAddedImage, imgTypeByBlipType(info.type), img);
    }
    else
    {
      ++m_lastAddedImage;
      MSPUB_DEBUG_MSG(("Image of unknown type at index 0x%x\n", m_lastAddedImage));
    }
    input->seek(info.contentsOffset + info.contentsLength, WPX_SEEK_SET);
  }
  return true;
}

bool libmspub::MSPUBParser::parseContents(WPXInputStream *input)
{
  MSPUB_DEBUG_MSG(("MSPUBParser::parseContents\n"));
  input->seek(0x1a, WPX_SEEK_SET);
  unsigned trailerOffset = readU32(input);
  MSPUB_DEBUG_MSG(("MSPUBParser: trailerOffset %.8x\n", trailerOffset));
  input->seek(trailerOffset, WPX_SEEK_SET);
  unsigned trailerLength = readU32(input);
  for (unsigned i=0; i<3; i++)
  {
    libmspub::MSPUBBlockInfo trailerPart = parseBlock(input);
    MSPUB_DEBUG_MSG(("Trailer SubBlock %i, startPosition 0x%lx, id %i, type 0x%x, dataLength 0x%lx\n", i+1, trailerPart.startPosition, trailerPart.id, trailerPart.type, trailerPart.dataLength));
    if (trailerPart.type == TRAILER_DIRECTORY)
    {

      while (stillReading(input, trailerPart.dataOffset + trailerPart.dataLength))
      {
        m_blockInfo.push_back(parseBlock(input));
        ++m_lastSeenSeqNum;
        if (m_blockInfo.back().type == GENERAL_CONTAINER)
        {
          if (parseContentChunkReference(input, m_blockInfo.back()))
          {
            if (m_contentChunks.size() > 1)
            {
              m_contentChunks[m_contentChunks.size() - 2].end
                = m_contentChunks.back().offset;
            }
          }
        }
        else(skipBlock(input, m_blockInfo.back()));
      }
      if (m_contentChunks.size() > 0)
      {
        m_contentChunks.back().end = trailerPart.dataOffset + trailerPart.dataLength;
      }
      if (!m_documentChunkIndex.is_initialized())
      {
        return false;
      }
      const ContentChunkReference &documentChunk = m_contentChunks.at(m_documentChunkIndex.get());
      for (unsigned i_pal = 0; i_pal < m_paletteChunkIndices.size(); ++i_pal)
      {
        const ContentChunkReference &paletteChunk = m_contentChunks.at(m_paletteChunkIndices[i_pal]);
        input->seek(paletteChunk.offset, WPX_SEEK_SET);
        if (! parsePaletteChunk(input, paletteChunk))
        {
          return false;
        }
      }
      input->seek(documentChunk.offset, WPX_SEEK_SET);
      if (!parseDocumentChunk(input, documentChunk))
      {
        return false;
      }
      for (unsigned i_page = 0; i_page < m_pageChunkIndices.size(); ++i_page)
      {
        const ContentChunkReference &pageChunk = m_contentChunks.at(m_pageChunkIndices[i_page]);
        input->seek(pageChunk.offset, WPX_SEEK_SET);
        if (!parsePageChunk(input, pageChunk))
        {
          return false;
        }
      }
    }
  }
  input->seek(trailerOffset + trailerLength, WPX_SEEK_SET);

  return true;
}

#ifdef DEBUG
bool libmspub::MSPUBParser::parseDocumentChunk(WPXInputStream *input, const ContentChunkReference &chunk)
#else
bool libmspub::MSPUBParser::parseDocumentChunk(WPXInputStream *input, const ContentChunkReference &)
#endif
{
  MSPUB_DEBUG_MSG(("parseDocumentChunk: offset 0x%lx, end 0x%lx\n", input->tell(), chunk.end));
  unsigned long begin = input->tell();
  unsigned long len = readU32(input);
  while (stillReading(input, begin + len))
  {
    libmspub::MSPUBBlockInfo info = parseBlock(input);
    if (info.id == DOCUMENT_SIZE)
    {
      while (stillReading(input, info.dataOffset + info.dataLength))
      {
        libmspub::MSPUBBlockInfo subInfo = parseBlock(input, true);
        if (subInfo.id == DOCUMENT_WIDTH)
        {
          m_collector->setWidthInEmu(subInfo.data);
        }
        else if (subInfo.id == DOCUMENT_HEIGHT)
        {
          m_collector->setHeightInEmu(subInfo.data);
        }
      }
    }
    else
    {
      skipBlock(input, info);
    }
  }
  return true; //FIXME: return false for failure
}


bool libmspub::MSPUBParser::parsePageChunk(WPXInputStream *input, const ContentChunkReference &chunk)
{
  MSPUB_DEBUG_MSG(("parsePageChunk: offset 0x%lx, end 0x%lx, seqnum 0x%x, parent 0x%x\n", input->tell(), chunk.end, chunk.seqNum, chunk.parentSeqNum));
  unsigned long length = readU32(input);
  PageType type = getPageTypeBySeqNum(chunk.seqNum);
  if (type == NORMAL || type == MASTER)
  {
    m_collector->addPage(chunk.seqNum);
    if (type == MASTER)
    {
      m_collector->designateMasterPage(chunk.seqNum);
    }
  }
  while (stillReading(input, chunk.offset + length))
  {
    libmspub::MSPUBBlockInfo info = parseBlock(input);
    if (info.id == PAGE_BG_SHAPE)
    {
      m_collector->setPageBgShape(chunk.seqNum, info.data);
    }
    else if (info.id == PAGE_SHAPES)
    {
      parseShapes(input, info, chunk.seqNum);
    }
    else
    {
      skipBlock(input, info);
    }
  }
  return true;
}

bool libmspub::MSPUBParser::parseShapes(WPXInputStream *input, libmspub::MSPUBBlockInfo info, unsigned pageSeqNum)
{
  MSPUB_DEBUG_MSG(("parseShapes: page seqnum 0x%x\n", pageSeqNum));
  while (stillReading(input, info.dataOffset + info.dataLength))
  {
    libmspub::MSPUBBlockInfo subInfo = parseBlock(input, true);
    if (subInfo.type == SHAPE_SEQNUM)
    {
      int index = -1;
      for (unsigned i = 0; i < m_shapeChunkIndices.size(); ++i)
      {
        if (m_contentChunks[m_shapeChunkIndices[i]].seqNum == subInfo.data)
        {
          index = m_shapeChunkIndices[i];
          break;
        }
      }
      if (index == -1)
      {
        MSPUB_DEBUG_MSG(("Shape of seqnum 0x%x not found!\n", subInfo.data));
      }
      else
      {
        const ContentChunkReference &ref = m_contentChunks[index];
        MSPUB_DEBUG_MSG(("Shape of seqnum 0x%x found\n", subInfo.data));
        unsigned long pos = input->tell();
        input->seek(ref.offset, WPX_SEEK_SET);
        bool parseWithoutDimensions = std::find(m_alternateShapeSeqNums.begin(), m_alternateShapeSeqNums.end(), subInfo.data) != m_alternateShapeSeqNums.end();
        parseShape(input, subInfo.data, pageSeqNum, parseWithoutDimensions, ref.type == GROUP);
        input->seek(pos, WPX_SEEK_SET);
      }
    }
  }
  return true;
}

bool libmspub::MSPUBParser::parseShape(WPXInputStream *input, unsigned seqNum, unsigned pageSeqNum, bool parseWithoutDimensions, bool isGroup)
{
  MSPUB_DEBUG_MSG(("parseShape: pageSeqNum 0x%x\n", pageSeqNum));
  unsigned long pos = input->tell();
  unsigned length = readU32(input);
  unsigned width = 0;
  unsigned height = 0;
  bool isText = false;
  unsigned textId = 0;
  while (stillReading(input, pos + length))
  {
    libmspub::MSPUBBlockInfo info = parseBlock(input, true);
    if (info.id == SHAPE_WIDTH)
    {
      width = info.data;
    }
    else if (info.id == SHAPE_HEIGHT)
    {
      height = info.data;
    }
    else if (info.id == SHAPE_TEXT_ID)
    {
      textId = info.data;
      isText = true;
    }
  }
  if (isGroup || (height > 0 && width > 0) || parseWithoutDimensions)
  {
    if (! isGroup)
    {
      if (isText)
      {
        m_collector->addTextShape(textId, seqNum, pageSeqNum);
      }
      m_collector->addShape(seqNum);
    }
    m_collector->setShapePage(seqNum, pageSeqNum);
  }
  else
  {
    MSPUB_DEBUG_MSG(("Height and width not both specified, ignoring. (Height: 0x%x, Width: 0x%x)\n", height, width));
  }
  return true;
}

libmspub::QuillChunkReference libmspub::MSPUBParser::parseQuillChunkReference(WPXInputStream *input)
{
  libmspub::QuillChunkReference ret;
  readU16(input); //FIXME: Can we do something sensible if this is not 0x18 ?
  char name[5];
  for (int i = 0; i < 4; ++i)
  {
    name[i] = (char)readU8(input);
  }
  name[4] = '\0';
  ret.name = name;
  ret.id = readU16(input);
  input->seek(input->tell() + 4, WPX_SEEK_SET); //Seek past what is normally 0x01000000. We don't know what this represents.
  char name2[5];
  for (int i = 0; i < 4; ++i)
  {
    name2[i] = (char)readU8(input);
  }
  name2[4] = '\0';
  ret.name2 = name2;
  ret.offset = readU32(input);
  ret.length = readU32(input);
  return ret;
}

bool libmspub::MSPUBParser::parseQuill(WPXInputStream *input)
{
  MSPUB_DEBUG_MSG(("MSPUBParser::parseQuill\n"));
  unsigned chunkReferenceListOffset = 0x18;
  std::list<QuillChunkReference> chunkReferences;
  while (chunkReferenceListOffset != 0xffffffff)
  {
    input->seek(chunkReferenceListOffset + 2, WPX_SEEK_SET);
    unsigned short numChunks = readU16(input);
    chunkReferenceListOffset = readU32(input);
    for (unsigned i = 0; i < numChunks; ++i)
    {
      libmspub::QuillChunkReference quillChunkReference = parseQuillChunkReference(input);
      chunkReferences.push_back(quillChunkReference);
    }
  }
  MSPUB_DEBUG_MSG(("Found %u Quill chunks\n", (unsigned)chunkReferences.size()));
  //Make sure we parse the STRS chunk before the TEXT chunk
  std::list<QuillChunkReference>::const_iterator textChunkReference = chunkReferences.end();
  bool parsedStrs = false;
  bool parsedSyid = false;
  bool parsedFdpc = false;
  bool parsedFdpp = false;
  bool parsedStsh = false;
  bool parsedFont = false;
  std::list<unsigned> textLengths;
  std::list<unsigned> textIDs;
  std::vector<TextSpanReference> spans;
  std::vector<TextParagraphReference> paras;
  unsigned whichStsh = 0;
  for (std::list<QuillChunkReference>::const_iterator i = chunkReferences.begin(); i != chunkReferences.end(); ++i)
  {
    if (i->name == "TEXT")
    {
      textChunkReference = i;
    }
    else if (i->name == "STRS")
    {
      input->seek(i->offset, WPX_SEEK_SET);
      unsigned numLengths = readU32(input); //Assuming the first DWORD is the number of children and that the next is the remaining length before children start. We are unsure that this is correct.
      input->seek(4 + i->offset + readU32(input), WPX_SEEK_SET);
      for (unsigned j = 0; j < numLengths; ++j)
      {
        textLengths.push_back(readU32(input));
      }
      parsedStrs = true;
    }
    else if (i->name == "SYID")
    {
      input->seek(i->offset, WPX_SEEK_SET);
      readU32(input); // Don't know what the first DWORD means.
      unsigned numIDs = readU32(input);
      for (unsigned j = 0; j < numIDs; ++j)
      {
        textIDs.push_back(readU32(input));
      }
      parsedSyid = true;
    }
    else if (i->name == "PL  ")
    {
      input->seek(i->offset, WPX_SEEK_SET);
      parseColors(input, *i);
    }
    else if (i->name == "FDPC")
    {
      input->seek(i->offset, WPX_SEEK_SET);
      std::vector<TextSpanReference> thisBlockSpans = parseCharacterStyles(input, *i);
      spans.insert(spans.end(), thisBlockSpans.begin(), thisBlockSpans.end());
      parsedFdpc = true;
    }
    else if (i->name == "FDPP")
    {
      input->seek(i->offset, WPX_SEEK_SET);
      std::vector<TextParagraphReference> thisBlockParas = parseParagraphStyles(input, *i);
      paras.insert(paras.end(), thisBlockParas.begin(), thisBlockParas.end());
      parsedFdpp = true;
    }
    else if (i->name == "STSH")
    {
      if (whichStsh++ == 1)
      {
        input->seek(i->offset, WPX_SEEK_SET);
        parseDefaultStyle(input, *i);
        parsedStsh = true;
      }
    }
    else if (i->name == "FONT")
    {
      input->seek(i->offset, WPX_SEEK_SET);
      parseFonts(input, *i);
      parsedFont = true;
    }
    if (parsedStrs && parsedSyid && parsedFdpc && parsedFdpp && parsedStsh && parsedFont && textChunkReference != chunkReferences.end())
    {
      input->seek(textChunkReference->offset, WPX_SEEK_SET);
      unsigned bytesRead = 0;
      std::vector<TextSpanReference>::iterator currentTextSpan = spans.begin();
      std::vector<TextParagraphReference>::iterator currentTextPara = paras.begin();
      for (std::list<unsigned>::const_iterator iter = textLengths.begin(), id = textIDs.begin(); iter != textLengths.end() && id != textIDs.end(); ++iter, ++id)
      {
        MSPUB_DEBUG_MSG(("Parsing a text block.\n"));
        std::vector<TextParagraph> readParas;
        std::vector<TextSpan> readSpans;
        std::vector<unsigned char> text;
        for (unsigned j = 0; j < *iter; ++j)
        {
          text.push_back(readU8(input));
          text.push_back(readU8(input));
          bytesRead += 2;
          if (bytesRead >= currentTextSpan->last - textChunkReference->offset)
          {
            if (!text.empty())
            {
              readSpans.push_back(TextSpan(text, currentTextSpan->charStyle));
              MSPUB_DEBUG_MSG(("Saw text span %d in the current text paragraph.\n", (unsigned)readSpans.size()));
            }
            ++currentTextSpan;
            text.clear();
          }
          if (bytesRead >= currentTextPara->last - textChunkReference->offset)
          {
            if (!text.empty())
            {
              readSpans.push_back(TextSpan(text, currentTextSpan->charStyle));
              MSPUB_DEBUG_MSG(("Saw text span %d in the current text paragraph.\n", (unsigned)readSpans.size()));
            }
            text.clear();
            if (!readSpans.empty())
            {
              readParas.push_back(TextParagraph(readSpans, currentTextPara->paraStyle));
              MSPUB_DEBUG_MSG(("Saw paragraph %d in the current text block.\n", (unsigned)readParas.size()));
            }
            ++currentTextPara;
            readSpans.clear();
          }
        }
        if (!readSpans.empty())
        {
          if (!text.empty())
          {
            readSpans.push_back(TextSpan(text, currentTextSpan->charStyle));
            MSPUB_DEBUG_MSG(("Saw text span %d in the current text paragraph.\n", (unsigned)readSpans.size()));
          }
          text.clear();
          readParas.push_back(TextParagraph(readSpans, currentTextPara->paraStyle));
          MSPUB_DEBUG_MSG(("Saw paragraph %d in the current text block.\n", (unsigned)readParas.size()));
        }
        m_collector->addTextString(readParas, *id);
      }
      textChunkReference = chunkReferences.end();
    }
  }
  return true;
}

void libmspub::MSPUBParser::parseFonts(WPXInputStream *input, const QuillChunkReference &)
{
  readU32(input);
  unsigned numElements = readU32(input);
  input->seek(input->tell() + 12 + 4 * numElements, WPX_SEEK_SET);
  for (unsigned i = 0; i < numElements; ++i)
  {
    unsigned short nameLength = readU16(input);
    std::vector<unsigned char> name;
    readNBytes(input, nameLength * 2, name);
    m_collector->addFont(name);
    readU32(input);
  }
}

void libmspub::MSPUBParser::parseDefaultStyle(WPXInputStream *input, const QuillChunkReference &chunk)
{
  readU32(input);
  unsigned numElements = readU32(input);
  input->seek(input->tell() + 12, WPX_SEEK_SET);
  std::vector<unsigned> offsets;
  offsets.reserve(numElements);
  for (unsigned i = 0; i < numElements; ++i)
  {
    offsets.push_back(readU32(input));
  }
  for (unsigned i = 0; i < numElements; ++i)
  {
    input->seek(chunk.offset + 20 + offsets[i], WPX_SEEK_SET);
    readU16(input);
    if (i % 2 == 0)
    {
      //FIXME: Does STSH2 hold information for associating style indices in FDPP to indices in STSH1 ?
      m_collector->addDefaultCharacterStyle(getCharacterStyle(input, true));
    }
    else
    {
      m_collector->addDefaultParagraphStyle(getParagraphStyle(input));
    }
  }
}


void libmspub::MSPUBParser::parseColors(WPXInputStream *input, const QuillChunkReference &)
{
  unsigned numEntries = readU32(input);
  input->seek(input->tell() + 8, WPX_SEEK_SET);
  for (unsigned i = 0; i < numEntries; ++i)
  {
    unsigned blocksOffset = input->tell();
    unsigned len = readU32(input);
    while (stillReading(input, blocksOffset + len))
    {
      MSPUBBlockInfo info = parseBlock(input, true);
      if (info.id == 0x01)
      {
        m_collector->addTextColor(ColorReference(info.data));
      }
    }
  }
}

std::vector<libmspub::MSPUBParser::TextParagraphReference> libmspub::MSPUBParser::parseParagraphStyles(WPXInputStream *input, const QuillChunkReference &chunk)
{
  std::vector<TextParagraphReference> ret;
  unsigned short numEntries = readU16(input);
  input->seek(input->tell() + 6, WPX_SEEK_SET);
  std::vector<unsigned> textOffsets;
  textOffsets.reserve(numEntries);
  std::vector<unsigned short> chunkOffsets;
  textOffsets.reserve(numEntries);
  for (unsigned short i = 0; i < numEntries; ++i)
  {
    textOffsets.push_back(readU32(input));
  }
  for (unsigned short i = 0; i < numEntries; ++i)
  {
    chunkOffsets.push_back(readU16(input));
  }
  unsigned currentSpanBegin = 0;
  for (unsigned short i = 0; i < numEntries; ++i)
  {
    input->seek(chunk.offset + chunkOffsets[i], WPX_SEEK_SET);
    ParagraphStyle style = getParagraphStyle(input);
    ret.push_back(TextParagraphReference(currentSpanBegin, textOffsets[i], style));
    currentSpanBegin = textOffsets[i] + 1;
  }
  return ret;
}

std::vector<libmspub::MSPUBParser::TextSpanReference> libmspub::MSPUBParser::parseCharacterStyles(WPXInputStream *input, const QuillChunkReference &chunk)
{
  unsigned short numEntries = readU16(input);
  input->seek(input->tell() + 6, WPX_SEEK_SET);
  std::vector<unsigned> textOffsets;
  textOffsets.reserve(numEntries);
  std::vector<unsigned short> chunkOffsets;
  chunkOffsets.reserve(numEntries);
  std::vector<TextSpanReference> ret;
  for (unsigned short i = 0; i < numEntries; ++i)
  {
    textOffsets.push_back(readU32(input));
  }
  for (unsigned short i = 0; i < numEntries; ++i)
  {
    chunkOffsets.push_back(readU16(input));
  }
  unsigned currentSpanBegin = 0;
  for (unsigned short i = 0; i < numEntries; ++i)
  {
    input->seek(chunk.offset + chunkOffsets[i], WPX_SEEK_SET);
    CharacterStyle style = getCharacterStyle(input);
    currentSpanBegin = textOffsets[i] + 1;
    ret.push_back(TextSpanReference(currentSpanBegin, textOffsets[i], style));
  }
  return ret;
}
libmspub::ParagraphStyle libmspub::MSPUBParser::getParagraphStyle(WPXInputStream *input)
{
  Alignment align = (Alignment)-1;
  unsigned lineSpacing = LINE_SPACING_UNIT;
  unsigned defaultCharStyleIndex = 0;
  unsigned spaceBeforeEmu = 0;
  unsigned spaceAfterEmu = 0;
  int firstLineIndentEmu = 0;
  unsigned leftIndentEmu = 0;
  unsigned rightIndentEmu = 0;
  unsigned offset = input->tell();
  unsigned len = readU32(input);
  while (stillReading(input, offset + len))
  {
    libmspub::MSPUBBlockInfo info = parseBlock(input, true);
    switch(info.id)
    {
    case PARAGRAPH_ALIGNMENT:
      align = (Alignment)(info.data & 0xFF); // Is this correct?
      break;
    case PARAGRAPH_DEFAULT_CHAR_STYLE:
      defaultCharStyleIndex = info.data;
      break;
    case PARAGRAPH_LINE_SPACING:
      lineSpacing = info.data;
      break;
    case PARAGRAPH_SPACE_BEFORE:
      spaceBeforeEmu = info.data;
      break;
    case PARAGRAPH_SPACE_AFTER:
      spaceAfterEmu = info.data;
      break;
    case PARAGRAPH_FIRST_LINE_INDENT:
      firstLineIndentEmu = (int)info.data;
      break;
    case PARAGRAPH_LEFT_INDENT:
      leftIndentEmu = info.data;
      break;
    case PARAGRAPH_RIGHT_INDENT:
      rightIndentEmu = info.data;
      break;
    default:
      break;
    }
  }
  return ParagraphStyle(align, defaultCharStyleIndex, lineSpacing, spaceBeforeEmu, spaceAfterEmu,
                        firstLineIndentEmu, leftIndentEmu, rightIndentEmu);
}

libmspub::CharacterStyle libmspub::MSPUBParser::getCharacterStyle(WPXInputStream *input, bool inStsh)
{
  bool seenUnderline = false, seenBold1 = false, seenBold2 = false, seenItalic1 = false, seenItalic2 = false;
  int textSize1 = -1, textSize2 = -1, colorIndex = -1;
  unsigned fontIndex = 0;
  unsigned offset = input->tell();
  unsigned len = readU32(input);
  while (stillReading(input, offset + len))
  {
    libmspub::MSPUBBlockInfo info = parseBlock(input, true);
    switch (info.id)
    {
    case BOLD_1_ID:
      seenBold1 = true;
      break;
    case BOLD_2_ID:
      seenBold2 = true;
      break;
    case ITALIC_1_ID:
      seenItalic1 = true;
      break;
    case ITALIC_2_ID:
      seenItalic2 = true;
      break;
    case UNDERLINE_ID:
      seenUnderline = true;
      break;
    case TEXT_SIZE_1_ID:
      textSize1 = info.data;
      break;
    case TEXT_SIZE_2_ID:
      textSize2 = info.data;
      break;
    case BARE_COLOR_INDEX_ID:
      colorIndex = info.data;
      break;
    case COLOR_INDEX_CONTAINER_ID:
      colorIndex = getColorIndex(input, info);
      break;
    case FONT_INDEX_CONTAINER_ID:
      if (! inStsh)
      {
        fontIndex = getFontIndex(input, info);
      }
      break;
    default:
      break;
    }
  }
  //FIXME: Figure out what textSize2 is used for. Can we find a document where it differs from textSize1 ?
  textSize2 = textSize1;
  return CharacterStyle(seenUnderline, seenItalic1 && seenItalic2, seenBold1 && seenBold2, textSize1 == textSize2 && textSize1 >= 0 ? (double)(textSize1 * POINTS_IN_INCH) / EMUS_IN_INCH : -1, getColorIndexByQuillEntry(colorIndex), fontIndex);
}

unsigned libmspub::MSPUBParser::getFontIndex(WPXInputStream *input, const MSPUBBlockInfo &info)
{
  MSPUB_DEBUG_MSG(("In getFontIndex\n"));
  input->seek(info.dataOffset + 4, WPX_SEEK_SET);
  while (stillReading(input, info.dataOffset + info.dataLength))
  {
    MSPUBBlockInfo subInfo = parseBlock(input, true);
    if (subInfo.type == GENERAL_CONTAINER)
    {
      input->seek(subInfo.dataOffset + 4, WPX_SEEK_SET);
      if (stillReading(input, subInfo.dataOffset + subInfo.dataLength))
      {
        MSPUBBlockInfo subSubInfo = parseBlock(input, true);
        return subSubInfo.data;
      }
    }
  }
  return 0;
}

int libmspub::MSPUBParser::getColorIndex(WPXInputStream *input, const MSPUBBlockInfo &info)
{
  input->seek(info.dataOffset + 4, WPX_SEEK_SET);
  while (stillReading(input, info.dataOffset + info.dataLength))
  {
    MSPUBBlockInfo subInfo = parseBlock(input, true);
    if (subInfo.id == COLOR_INDEX_ID)
    {
      skipBlock(input, info);
      MSPUB_DEBUG_MSG(("Found color index 0x%x\n", (unsigned)subInfo.data));
      return subInfo.data;
    }
  }
  MSPUB_DEBUG_MSG(("Failed to find color index!\n"));
  return -1;
}

bool libmspub::MSPUBParser::parseEscher(WPXInputStream *input)
{
  MSPUB_DEBUG_MSG(("MSPUBParser::parseEscher\n"));
  libmspub::EscherContainerInfo fakeroot;
  fakeroot.initial = 0;
  fakeroot.type = 0;
  fakeroot.contentsOffset = input->tell();
  fakeroot.contentsLength = (unsigned long)-1; //FIXME: Get the actual length
  libmspub::EscherContainerInfo dg, dgg;
  //Note: this assumes that dgg comes before any dg with images.
  if (findEscherContainer(input, fakeroot, dgg, OFFICE_ART_DGG_CONTAINER))
  {
    libmspub::EscherContainerInfo bsc;
    if (findEscherContainer(input, fakeroot, bsc, OFFICE_ART_B_STORE_CONTAINER))
    {
      unsigned short currentDelayIndex = 1;
      while (stillReading(input, bsc.contentsOffset + bsc.contentsLength))
      {
        unsigned begin = input->tell();
        input->seek(begin + 10, WPX_SEEK_SET);
        if (! (readU32(input) == 0 && readU32(input) == 0 && readU32(input) == 0 && readU32(input) == 0))
        {
          m_escherDelayIndices.push_back(currentDelayIndex++);
        }
        else
        {
          m_escherDelayIndices.push_back(-1);
        }
        input->seek(begin + 44, WPX_SEEK_SET);
      }
    }
    input->seek(dgg.contentsOffset + dgg.contentsLength + getEscherElementTailLength(OFFICE_ART_DGG_CONTAINER), WPX_SEEK_SET);
  }
  while (findEscherContainer(input, fakeroot, dg, OFFICE_ART_DG_CONTAINER))
  {
    libmspub::EscherContainerInfo spgr;
    while (findEscherContainer(input, dg, spgr, OFFICE_ART_SPGR_CONTAINER))
    {
      Coordinate c1, c2;
      parseShapeGroup(input, spgr, true, c1, c2);
    }
    input->seek(input->tell() + getEscherElementTailLength(OFFICE_ART_DG_CONTAINER), WPX_SEEK_SET);
  }
  return true;
}

void libmspub::MSPUBParser::parseShapeGroup(WPXInputStream *input, const EscherContainerInfo &spgr, bool topLevel, Coordinate parentCoordinateSystem, Coordinate parentGroupAbsoluteCoord)
{
  libmspub::EscherContainerInfo shapeOrGroup;
  std::set<unsigned short> types;
  types.insert(OFFICE_ART_SPGR_CONTAINER);
  types.insert(OFFICE_ART_SP_CONTAINER);
  while (findEscherContainerWithTypeInSet(input, spgr, shapeOrGroup, types))
  {
    switch (shapeOrGroup.type)
    {
    case OFFICE_ART_SPGR_CONTAINER:
      m_collector->beginGroup();
      parseShapeGroup(input, shapeOrGroup, false, parentCoordinateSystem, parentGroupAbsoluteCoord);
      m_collector->endGroup();
      break;
    case OFFICE_ART_SP_CONTAINER:
      parseEscherShape(input, shapeOrGroup, topLevel, parentCoordinateSystem, parentGroupAbsoluteCoord);
      break;
    }
    input->seek(shapeOrGroup.contentsOffset + shapeOrGroup.contentsLength + getEscherElementTailLength(shapeOrGroup.type), WPX_SEEK_SET);
  }
}

void libmspub::MSPUBParser::parseEscherShape(WPXInputStream *input, const EscherContainerInfo &sp, bool topLevel, Coordinate &parentCoordinateSystem, Coordinate &parentGroupAbsoluteCoord)
{
  Coordinate thisParentCoordinateSystem = parentCoordinateSystem;
  bool definesRelativeCoordinates = false;
  libmspub::EscherContainerInfo cData;
  libmspub::EscherContainerInfo cAnchor;
  libmspub::EscherContainerInfo cFopt;
  libmspub::EscherContainerInfo cTertiaryFopt;
  libmspub::EscherContainerInfo cFsp;
  libmspub::EscherContainerInfo cFspgr;
  unsigned shapeFlags = 0;
  bool isGroupLeader = false;
  ShapeType st = RECTANGLE;
  if (findEscherContainer(input, sp, cFspgr, OFFICE_ART_FSPGR))
  {
    input->seek(cFspgr.contentsOffset, WPX_SEEK_SET);
    parentCoordinateSystem.m_xs = readU32(input);
    parentCoordinateSystem.m_ys = readU32(input);
    parentCoordinateSystem.m_xe = readU32(input);
    parentCoordinateSystem.m_ye = readU32(input);
    definesRelativeCoordinates = true;
  }
  input->seek(sp.contentsOffset, WPX_SEEK_SET);
  if (findEscherContainer(input, sp, cFsp, OFFICE_ART_FSP))
  {
    st = (ShapeType)(cFsp.initial >> 4);
    std::map<unsigned short, unsigned> fspData = extractEscherValues(input, cFsp);
    input->seek(cFsp.contentsOffset + 4, WPX_SEEK_SET);
    shapeFlags = readU32(input);
    isGroupLeader = shapeFlags & SF_GROUP;
  }
  input->seek(sp.contentsOffset, WPX_SEEK_SET);
  if (findEscherContainer(input, sp, cData, OFFICE_ART_CLIENT_DATA))
  {
    std::map<unsigned short, unsigned> dataValues = extractEscherValues(input, cData);
    unsigned *shapeSeqNum = getIfExists(dataValues, FIELDID_SHAPE_ID);
    if (shapeSeqNum)
    {
      m_collector->setShapeType(*shapeSeqNum, st);
      m_collector->setShapeFlip(*shapeSeqNum, shapeFlags & SF_FLIP_V, shapeFlags & SF_FLIP_H);
      input->seek(sp.contentsOffset, WPX_SEEK_SET);
      if (isGroupLeader)
      {
        m_collector->setCurrentGroupSeqNum(*shapeSeqNum);
      }
      else
      {
        m_collector->setShapeOrder(*shapeSeqNum);
      }
      std::set<unsigned short> anchorTypes;
      anchorTypes.insert(OFFICE_ART_CLIENT_ANCHOR);
      anchorTypes.insert(OFFICE_ART_CHILD_ANCHOR);
      bool foundAnchor;
      bool rotated90 = false;
      if ((foundAnchor = findEscherContainerWithTypeInSet(input, sp, cAnchor, anchorTypes)) || isGroupLeader)
      {
        MSPUB_DEBUG_MSG(("Found Escher data for %s of seqnum 0x%x\n", isGroupLeader ? "group" : "shape", *shapeSeqNum));
        input->seek(sp.contentsOffset, WPX_SEEK_SET);
        if (findEscherContainer(input, sp, cFopt, OFFICE_ART_FOPT))
        {
          std::map<unsigned short, unsigned> foptValues = extractEscherValues(input, cFopt);
          unsigned *pxId = getIfExists(foptValues, FIELDID_PXID);
          if (pxId)
          {
            MSPUB_DEBUG_MSG(("Current Escher shape has pxId %d\n", *pxId));
            if (*pxId <= m_escherDelayIndices.size() && m_escherDelayIndices[*pxId - 1] >= 0)
            {
              m_collector->setShapeImgIndex(*shapeSeqNum, m_escherDelayIndices[*pxId - 1]);
            }
            else
            {
              MSPUB_DEBUG_MSG(("Couldn't find corresponding escherDelay index\n"));
            }
          }
          unsigned *ptr_lineColor = getIfExists(foptValues, FIELDID_LINE_COLOR);
          unsigned *ptr_lineFlags = getIfExists(foptValues, FIELDID_LINE_STYLE_BOOL_PROPS);
          bool useLine = lineExistsByFlagPointer(ptr_lineFlags);
          bool skipIfNotBg = false;
          boost::shared_ptr<Fill> ptr_fill = getNewFill(foptValues, skipIfNotBg);
          if (ptr_lineColor && useLine)
          {
            unsigned *ptr_lineWidth = getIfExists(foptValues, FIELDID_LINE_WIDTH);
            unsigned lineWidth = ptr_lineWidth ? *ptr_lineWidth : 9525;
            m_collector->addShapeLine(*shapeSeqNum, Line(ColorReference(*ptr_lineColor), lineWidth, true));
          }
          else
          {
            input->seek(sp.contentsOffset, WPX_SEEK_SET);
            if (findEscherContainer(input, sp, cTertiaryFopt, OFFICE_ART_TERTIARY_FOPT))
            {
              std::map<unsigned short, unsigned> tertiaryFoptValues = extractEscherValues(input, cTertiaryFopt);
              unsigned *ptr_tertiaryLineFlags = getIfExists(tertiaryFoptValues, FIELDID_LINE_STYLE_BOOL_PROPS);
              if (lineExistsByFlagPointer(ptr_tertiaryLineFlags))
              {
                unsigned *ptr_topColor = getIfExists(tertiaryFoptValues, FIELDID_LINE_TOP_COLOR);
                unsigned *ptr_topWidth = getIfExists(tertiaryFoptValues, FIELDID_LINE_TOP_WIDTH);
                unsigned *ptr_topFlags = getIfExists(tertiaryFoptValues, FIELDID_LINE_TOP_BOOL_PROPS);
                unsigned *ptr_rightColor = getIfExists(tertiaryFoptValues, FIELDID_LINE_RIGHT_COLOR);
                unsigned *ptr_rightWidth = getIfExists(tertiaryFoptValues, FIELDID_LINE_RIGHT_WIDTH);
                unsigned *ptr_rightFlags = getIfExists(tertiaryFoptValues, FIELDID_LINE_RIGHT_BOOL_PROPS);
                unsigned *ptr_bottomColor = getIfExists(tertiaryFoptValues, FIELDID_LINE_BOTTOM_COLOR);
                unsigned *ptr_bottomWidth = getIfExists(tertiaryFoptValues, FIELDID_LINE_BOTTOM_WIDTH);
                unsigned *ptr_bottomFlags = getIfExists(tertiaryFoptValues, FIELDID_LINE_BOTTOM_BOOL_PROPS);
                unsigned *ptr_leftColor = getIfExists(tertiaryFoptValues, FIELDID_LINE_LEFT_COLOR);
                unsigned *ptr_leftWidth = getIfExists(tertiaryFoptValues, FIELDID_LINE_LEFT_WIDTH);
                unsigned *ptr_leftFlags = getIfExists(tertiaryFoptValues, FIELDID_LINE_LEFT_BOOL_PROPS);

                bool topExists = ptr_topColor && lineExistsByFlagPointer(ptr_topFlags);
                bool rightExists = ptr_rightColor && lineExistsByFlagPointer(ptr_rightFlags);
                bool bottomExists = ptr_bottomColor && lineExistsByFlagPointer(ptr_bottomFlags);
                bool leftExists = ptr_leftColor && lineExistsByFlagPointer(ptr_leftFlags);

                m_collector->addShapeLine(*shapeSeqNum,
                                          topExists ? Line(ColorReference(*ptr_topColor), ptr_topWidth ? *ptr_topWidth : 9525, true) :
                                            Line(ColorReference(0), 0, false));
                m_collector->addShapeLine(*shapeSeqNum,
                                          rightExists ? Line(ColorReference(*ptr_rightColor), ptr_rightWidth ? *ptr_rightWidth : 9525, true) :
                                            Line(ColorReference(0), 0, false));
                m_collector->addShapeLine(*shapeSeqNum,
                                          bottomExists ? Line(ColorReference(*ptr_bottomColor), ptr_bottomWidth ? *ptr_bottomWidth : 9525, true) :
                                            Line(ColorReference(0), 0, false));
                m_collector->addShapeLine(*shapeSeqNum,
                                          leftExists ? Line(ColorReference(*ptr_leftColor), ptr_leftWidth ? *ptr_leftWidth : 9525, true) :
                                            Line(ColorReference(0), 0, false));

                // Amazing feat of Microsoft engineering:
                // The detailed interaction of four flags describes ONE true/false property!

                if (ptr_leftFlags &&
                    (*ptr_leftFlags & FLAG_USE_LEFT_INSET_PEN) &&
                    (!(*ptr_leftFlags & FLAG_USE_LEFT_INSET_PEN_OK) || (*ptr_leftFlags & FLAG_LEFT_INSET_PEN_OK)) &&
                    (*ptr_leftFlags & FLAG_LEFT_INSET_PEN))
                {
                  m_collector->setShapeBorderPosition(*shapeSeqNum, INSIDE_SHAPE);
                }
                else
                {
                  m_collector->setShapeBorderPosition(*shapeSeqNum, HALF_INSIDE_SHAPE);
                }
              }
            }
          }
          if (ptr_fill)
          {
            m_collector->setShapeFill(*shapeSeqNum, ptr_fill, skipIfNotBg);
          }
          int *ptr_adjust1 = (int *)getIfExists(foptValues, FIELDID_ADJUST_VALUE_1);
          int *ptr_adjust2 = (int *)getIfExists(foptValues, FIELDID_ADJUST_VALUE_2);
          int *ptr_adjust3 = (int *)getIfExists(foptValues, FIELDID_ADJUST_VALUE_3);
          if (ptr_adjust1)
          {
            m_collector->setAdjustValue(*shapeSeqNum, 0, *ptr_adjust1);
          }
          if (ptr_adjust2)
          {
            m_collector->setAdjustValue(*shapeSeqNum, 1, *ptr_adjust2);
          }
          if (ptr_adjust3)
          {
            m_collector->setAdjustValue(*shapeSeqNum, 2, *ptr_adjust3);
          }
          int *ptr_rotation = (int *)getIfExists(foptValues, FIELDID_ROTATION);
          if (ptr_rotation)
          {
            double rotation = doubleModulo(toFixedPoint(*ptr_rotation), 360);
            m_collector->setShapeRotation(*shapeSeqNum, short(rotation));
            //FIXME : make MSPUBCollector handle double shape rotations
            rotated90 = (rotation >= 45 && rotation < 135) || (rotation >= 225 && rotation < 315);

          }
          unsigned *ptr_left = getIfExists(foptValues, FIELDID_DY_TEXT_LEFT);
          unsigned *ptr_top = getIfExists(foptValues, FIELDID_DY_TEXT_TOP);
          unsigned *ptr_right = getIfExists(foptValues, FIELDID_DY_TEXT_RIGHT);
          unsigned *ptr_bottom = getIfExists(foptValues, FIELDID_DY_TEXT_BOTTOM);
          m_collector->setShapeMargins(*shapeSeqNum, ptr_left ? *ptr_left : DEFAULT_MARGIN,
                                       ptr_top ? *ptr_top : DEFAULT_MARGIN,
                                       ptr_right ? *ptr_right : DEFAULT_MARGIN,
                                       ptr_bottom ? *ptr_bottom : DEFAULT_MARGIN);
        }
        if (foundAnchor)
        {
          Coordinate absolute;
          if (cAnchor.type == OFFICE_ART_CLIENT_ANCHOR)
          {
            std::map<unsigned short, unsigned> anchorData = extractEscherValues(input, cAnchor);
            absolute = Coordinate(anchorData[FIELDID_XS],
                                  anchorData[FIELDID_YS], anchorData[FIELDID_XE],
                                  anchorData[FIELDID_YE]);
          }
          else if (cAnchor.type == OFFICE_ART_CHILD_ANCHOR)
          {
            input->seek(cAnchor.contentsOffset, WPX_SEEK_SET);
            int coordSystemWidth = thisParentCoordinateSystem.m_xe - thisParentCoordinateSystem.m_xs;
            int coordSystemHeight = thisParentCoordinateSystem.m_ye - thisParentCoordinateSystem.m_ys;
            int groupWidth = parentGroupAbsoluteCoord.m_xe - parentGroupAbsoluteCoord.m_xs;
            int groupHeight = parentGroupAbsoluteCoord.m_ye - parentGroupAbsoluteCoord.m_ys;
            double widthScale = (double)groupWidth / coordSystemWidth;
            double heightScale = (double)groupHeight / coordSystemHeight;
            int xs = (readU32(input) - thisParentCoordinateSystem.m_xs) * widthScale + parentGroupAbsoluteCoord.m_xs;
            int ys = (readU32(input) - thisParentCoordinateSystem.m_ys) * heightScale + parentGroupAbsoluteCoord.m_ys;
            int xe = (readU32(input) - thisParentCoordinateSystem.m_xs) * widthScale + parentGroupAbsoluteCoord.m_xs;
            int ye = (readU32(input) - thisParentCoordinateSystem.m_ys) * heightScale + parentGroupAbsoluteCoord.m_ys;

            absolute = Coordinate(xs, ys, xe, ye);
          }
          if (rotated90)
          {
            int initialX = absolute.m_xs;
            int initialY = absolute.m_ys;
            int initialWidth = absolute.m_xe - absolute.m_xs;
            int initialHeight = absolute.m_ye - absolute.m_ys;
            int centerX = initialX + initialWidth / 2;
            int centerY = initialY + initialHeight / 2;
            int xs = centerX - initialHeight / 2;
            int ys = centerY - initialWidth / 2;
            int xe = xs + initialHeight;
            int ye = ys + initialWidth;
            absolute = Coordinate(xs, ys, xe, ye);
          }
          m_collector->setShapeCoordinatesInEmu(*shapeSeqNum,
                                                absolute.m_xs,
                                                absolute.m_ys,
                                                absolute.m_xe,
                                                absolute.m_ye);
          if (definesRelativeCoordinates)
          {
            parentGroupAbsoluteCoord = absolute;
          }
        }
      }
      if (!topLevel)
      {
        m_collector->addShape(*shapeSeqNum);
      }
    }
  }
}

boost::shared_ptr<libmspub::Fill> libmspub::MSPUBParser::getNewFill(const std::map<unsigned short, unsigned> &foptProperties,
    bool &skipIfNotBg)
{
  const FillType *ptr_fillType = (FillType *)getIfExists_const(foptProperties, FIELDID_FILL_TYPE);
  FillType fillType = ptr_fillType ? *ptr_fillType : SOLID;
  switch (fillType)
  {
  case SOLID:
  {
    const unsigned *ptr_fillColor = getIfExists_const(foptProperties, FIELDID_FILL_COLOR);
    const unsigned *ptr_fieldStyleProps = getIfExists_const(foptProperties, FIELDID_FIELD_STYLE_BOOL_PROPS);
    skipIfNotBg = ptr_fieldStyleProps && (*ptr_fieldStyleProps & 0xF0) == 0;
    if (ptr_fillColor && !skipIfNotBg)
    {
      const unsigned *ptr_fillOpacity = getIfExists_const(foptProperties, FIELDID_FILL_OPACITY);
      return boost::shared_ptr<Fill>(new SolidFill(ColorReference(*ptr_fillColor), ptr_fillOpacity ? (double)(*ptr_fillOpacity) / 0xFFFF : 1, m_collector));
    }
    return boost::shared_ptr<Fill>();
  }
  case GRADIENT: //FIXME: The handling of multi-color gradients here is quite bad.
  {
    int angle;
    const int *ptr_angle = (const int *)getIfExists_const(foptProperties, FIELDID_FILL_ANGLE);
    const unsigned *ptr_fillColor = getIfExists_const(foptProperties, FIELDID_FILL_COLOR);
    const unsigned *ptr_fillBackColor = getIfExists_const(foptProperties, FIELDID_FILL_BACK_COLOR);
    unsigned fill = ptr_fillColor ? *ptr_fillColor : 0x00FFFFFFF;
    unsigned fillBack = ptr_fillBackColor ? *ptr_fillBackColor : 0x00FFFFFF;
    ColorReference firstColor(fill, fill);
    ColorReference secondColor(fill, fillBack);
    const unsigned *ptr_fillOpacity = getIfExists_const(foptProperties, FIELDID_FILL_OPACITY);
    const unsigned *ptr_fillBackOpacity = getIfExists_const(foptProperties, FIELDID_FILL_BACK_OPACITY);
    const unsigned *ptr_fillFocus = getIfExists_const(foptProperties, FIELDID_FILL_FOCUS);
    short fillFocus = ptr_fillFocus ? ((int)(*ptr_fillFocus) << 16) >> 16 : 0;
    angle = ptr_angle ? *ptr_angle : 0;
    angle >>= 16; //it's actually only 16 bits
    // Don't try to figure out what sense the following switch statement makes.
    // The angles are just offset by 90 degrees in the file format in some cases.
    // It seems totally arbitrary -- maybe an MS bug ?
    switch (angle)
    {
    case -135:
      angle = -45;
      break;
    case -45:
      angle = 225;
      break;
    default:
      break;
    }

    boost::shared_ptr<GradientFill> ret(new GradientFill(m_collector, angle));
    if (fillFocus ==  0)
    {
      ret->addColor(firstColor, 0, ptr_fillOpacity ? (double)(*ptr_fillOpacity) / 0xFFFF : 1);
      ret->addColor(secondColor, 100, ptr_fillBackOpacity ? (double)(*ptr_fillBackOpacity) / 0xFFFF : 1);
    }
    else if (fillFocus == 100)
    {
      ret->addColor(secondColor, 0, ptr_fillBackOpacity ? (double)(*ptr_fillBackOpacity) / 0xFFFF : 1);
      ret->addColor(firstColor, 100, ptr_fillOpacity ? (double)(*ptr_fillOpacity) / 0xFFFF : 1);
    }
    else if (fillFocus > 0)
    {
      ret->addColor(firstColor, 0, ptr_fillOpacity ? (double)(*ptr_fillOpacity) / 0xFFFF : 1);
      ret->addColor(secondColor, fillFocus, ptr_fillBackOpacity ? (double)(*ptr_fillBackOpacity) / 0xFFFF : 1);
      ret->addColor(firstColor, 100, ptr_fillOpacity ? (double)(*ptr_fillOpacity) / 0xFFFF : 1);
    }
    else if (fillFocus < 0)
    {
      ret->addColor(secondColor, 0, ptr_fillBackOpacity ? (double)(*ptr_fillBackOpacity) / 0xFFFF : 1);
      ret->addColor(firstColor, 100 + fillFocus, ptr_fillOpacity ? (double)(*ptr_fillOpacity) / 0xFFFF : 1);
      ret->addColor(secondColor, 100, ptr_fillBackOpacity ? (double)(*ptr_fillBackOpacity) / 0xFFFF : 1);
    }
    return ret;
  }
  case TEXTURE:
  case BITMAP:
  {
    const unsigned *ptr_bgPxId = getIfExists_const(foptProperties, FIELDID_BG_PXID);
    if (ptr_bgPxId && *ptr_bgPxId <= m_escherDelayIndices.size() && m_escherDelayIndices[*ptr_bgPxId - 1] >= 0)
    {
      return boost::shared_ptr<Fill>(new ImgFill(m_escherDelayIndices[*ptr_bgPxId - 1], m_collector, fillType == TEXTURE));
    }
    return boost::shared_ptr<Fill>();
  }
  case PATTERN:
  {
    const unsigned *ptr_bgPxId = getIfExists_const(foptProperties, FIELDID_BG_PXID);
    const unsigned *ptr_fillColor = getIfExists_const(foptProperties, FIELDID_FILL_COLOR);
    const unsigned *ptr_fillBackColor = getIfExists_const(foptProperties, FIELDID_FILL_BACK_COLOR);
    ColorReference fill = ptr_fillColor ? ColorReference(*ptr_fillColor) : ColorReference(0x00FFFFFF);
    ColorReference back = ptr_fillBackColor ? ColorReference(*ptr_fillBackColor) : ColorReference(0x08000000);
    if (ptr_bgPxId && *ptr_bgPxId <= m_escherDelayIndices.size() && m_escherDelayIndices[*ptr_bgPxId - 1 ] >= 0)
    {
      return boost::shared_ptr<Fill>(new PatternFill(m_escherDelayIndices[*ptr_bgPxId - 1], m_collector, fill, back));
    }
  }
  default:
    return boost::shared_ptr<Fill>();
  }
}

unsigned libmspub::MSPUBParser::getEscherElementTailLength(unsigned short type)
{
  switch (type)
  {
  case OFFICE_ART_DGG_CONTAINER:
  case OFFICE_ART_DG_CONTAINER:
    return 4;
  default:
    return 0;
  }
}

unsigned libmspub::MSPUBParser::getEscherElementAdditionalHeaderLength(unsigned short type)
{
  switch (type)
  {
  case OFFICE_ART_CLIENT_ANCHOR:
  case OFFICE_ART_CLIENT_DATA: //account for the fact that the length appears twice, for whatever reason
    return 4;
  }
  return 0;
}

bool libmspub::MSPUBParser::findEscherContainerWithTypeInSet(WPXInputStream *input, const libmspub::EscherContainerInfo &parent, libmspub::EscherContainerInfo &out, std::set<unsigned short> types)
{
  while (stillReading(input, parent.contentsOffset + parent.contentsLength))
  {
    libmspub::EscherContainerInfo next = parseEscherContainer(input);
    if (types.find(next.type) != types.end())
    {
      out = next;
      return true;
    }
    input->seek(next.contentsOffset + next.contentsLength + getEscherElementTailLength(next.type), WPX_SEEK_SET);
  }
  return false;
}

bool libmspub::MSPUBParser::findEscherContainer(WPXInputStream *input, const libmspub::EscherContainerInfo &parent, libmspub::EscherContainerInfo &out, unsigned short desiredType)
{
  MSPUB_DEBUG_MSG(("At offset 0x%lx, attempting to find escher container of type 0x%x\n", input->tell(), desiredType));
  while (stillReading(input, parent.contentsOffset + parent.contentsLength))
  {
    libmspub::EscherContainerInfo next = parseEscherContainer(input);
    if (next.type == desiredType)
    {
      out = next;
      return true;
    }
    input->seek(next.contentsOffset + next.contentsLength + getEscherElementTailLength(next.type), WPX_SEEK_SET);
  }
  return false;
}

std::map<unsigned short, unsigned> libmspub::MSPUBParser::extractEscherValues(WPXInputStream *input, const libmspub::EscherContainerInfo &record)
{
  std::map<unsigned short, unsigned> ret;
  input->seek(record.contentsOffset + getEscherElementAdditionalHeaderLength(record.type), WPX_SEEK_SET);
  while (stillReading(input, record.contentsOffset + record.contentsLength))
  {
    unsigned short id = readU16(input);
    unsigned value = readU32(input);
    ret[id] = value;
  }
  return ret;
}


bool libmspub::MSPUBParser::parseContentChunkReference(WPXInputStream *input, const libmspub::MSPUBBlockInfo block)
{
  //input should be at block.dataOffset + 4 , that is, at the beginning of the list of sub-blocks
  MSPUB_DEBUG_MSG(("Parsing chunk reference 0x%x\n", m_lastSeenSeqNum));
  libmspub::MSPUBContentChunkType type = (libmspub::MSPUBContentChunkType)0;
  unsigned long offset = 0;
  unsigned parentSeqNum = 0;
  bool seenType = false;
  bool seenOffset = false;
  bool seenParentSeqNum = false;
  while (stillReading(input, block.dataOffset + block.dataLength))
  {
    libmspub::MSPUBBlockInfo subBlock = parseBlock(input, true);
    //FIXME: Warn if multiple of these blocks seen.
    if (subBlock.id == CHUNK_TYPE)
    {
      type = (libmspub::MSPUBContentChunkType)subBlock.data;
      seenType = true;
    }
    else if (subBlock.id == CHUNK_OFFSET)
    {
      offset = subBlock.data;
      seenOffset = true;
    }
    else if (subBlock.id == CHUNK_PARENT_SEQNUM)
    {
      parentSeqNum = subBlock.data;
      seenParentSeqNum = true;
    }
  }
  if (seenType && seenOffset) //FIXME: What if there is an offset, but not a type? Should we still set the end of the preceding chunk to that offset?
  {
    if (type == PAGE)
    {
      MSPUB_DEBUG_MSG(("page chunk: offset 0x%lx, seqnum 0x%x\n", offset, m_lastSeenSeqNum));
      m_contentChunks.push_back(ContentChunkReference(type, offset, 0, m_lastSeenSeqNum, seenParentSeqNum ? parentSeqNum : 0));
      m_pageChunkIndices.push_back(unsigned(m_contentChunks.size() - 1));
      return true;
    }
    else if (type == DOCUMENT)
    {
      MSPUB_DEBUG_MSG(("document chunk: offset 0x%lx, seqnum 0x%x\n", offset, m_lastSeenSeqNum));
      m_contentChunks.push_back(ContentChunkReference(type, offset, 0, m_lastSeenSeqNum, seenParentSeqNum ? parentSeqNum : 0));
      m_documentChunkIndex = unsigned(m_contentChunks.size() - 1);
      return true;
    }
    else if (type == SHAPE || type == ALTSHAPE || type == GROUP)
    {
      MSPUB_DEBUG_MSG(("shape chunk: offset 0x%lx, seqnum 0x%x, parent seqnum: 0x%x\n", offset, m_lastSeenSeqNum, parentSeqNum));
      m_contentChunks.push_back(ContentChunkReference(type, offset, 0, m_lastSeenSeqNum, seenParentSeqNum ? parentSeqNum : 0));
      m_shapeChunkIndices.push_back(unsigned(m_contentChunks.size() - 1));
      if (type == ALTSHAPE)
      {
        m_alternateShapeSeqNums.push_back(m_lastSeenSeqNum);
      }
      return true;
    }
    else if (type == PALETTE)
    {
      m_contentChunks.push_back(ContentChunkReference(type, offset, 0, m_lastSeenSeqNum, seenParentSeqNum ? parentSeqNum : 0));
      m_paletteChunkIndices.push_back(unsigned(m_contentChunks.size() - 1));
      return true;
    }
    m_contentChunks.push_back(ContentChunkReference(type, offset, 0, m_lastSeenSeqNum, seenParentSeqNum ? parentSeqNum : 0));
    m_unknownChunkIndices.push_back(unsigned(m_contentChunks.size() - 1));
  }
  return false;
}

bool libmspub::MSPUBParser::isBlockDataString(unsigned type)
{
  return type == STRING_CONTAINER;
}
void libmspub::MSPUBParser::skipBlock(WPXInputStream *input, libmspub::MSPUBBlockInfo block)
{
  input->seek(block.dataOffset + block.dataLength, WPX_SEEK_SET);
}

libmspub::EscherContainerInfo libmspub::MSPUBParser::parseEscherContainer(WPXInputStream *input)
{
  libmspub::EscherContainerInfo info;
  info.initial = readU16(input);
  info.type = readU16(input);
  info.contentsLength = readU32(input);
  info.contentsOffset = input->tell();
  MSPUB_DEBUG_MSG(("Parsed escher container: type 0x%x, contentsOffset 0x%lx, contentsLength 0x%lx\n", info.type, info.contentsOffset, info.contentsLength));
  return info;
}

libmspub::MSPUBBlockInfo libmspub::MSPUBParser::parseBlock(WPXInputStream *input, bool skipHierarchicalData)
{
  libmspub::MSPUBBlockInfo info;
  info.startPosition = input->tell();
  info.id = (MSPUBBlockID)readU8(input);
  info.type = (MSPUBBlockType)readU8(input);
  info.dataOffset = input->tell();
  int len = getBlockDataLength(info.type);
  bool varLen = len < 0;
  if (varLen)
  {
    info.dataLength = readU32(input);
    if (isBlockDataString(info.type))
    {
      info.stringData = std::vector<unsigned char>();
      readNBytes(input, info.dataLength - 4, info.stringData);
    }
    else if (skipHierarchicalData)
    {
      skipBlock(input, info);
    }
    info.data = 0;
  }
  else
  {
    info.dataLength = len;
    switch (info.dataLength)
    {
    case 1:
      info.data = readU8(input);
      break;
    case 2:
      info.data = readU16(input);
      break;
    case 4:
      info.data = readU32(input);
      break;
    case 8:
    case 16:
      //FIXME: Not doing anything with this data for now.
      skipBlock(input, info);
    default:
      info.data = 0;
    }
  }
  MSPUB_DEBUG_MSG(("parseBlock dataOffset 0x%lx, id 0x%x, type 0x%x, dataLength 0x%lx, integral data 0x%x\n", info.dataOffset, info.id, info.type, info.dataLength, info.data));
  return info;
}

libmspub::PageType libmspub::MSPUBParser::getPageTypeBySeqNum(unsigned seqNum)
{
  switch(seqNum)
  {
  case 0x107:
    return MASTER;
  case 0x10d:
  case 0x110:
  case 0x113:
  case 0x117:
    return DUMMY_PAGE;
  default:
    return NORMAL;
  }
}

bool libmspub::MSPUBParser::parsePaletteChunk(WPXInputStream *input, const ContentChunkReference &chunk)
{
  unsigned length = readU32(input);
  while (stillReading(input, chunk.offset + length))
  {
    MSPUBBlockInfo info = parseBlock(input);
    if (info.type == 0xA0)
    {
      while (stillReading(input, info.dataOffset + info.dataLength))
      {
        MSPUBBlockInfo subInfo = parseBlock(input);
        if (subInfo.type == GENERAL_CONTAINER)
        {
          parsePaletteEntry(input, subInfo);
        }
        skipBlock(input, subInfo);
      }
    }
    skipBlock(input, info);
  }
  return true;
}

void libmspub::MSPUBParser::parsePaletteEntry(WPXInputStream *input, MSPUBBlockInfo info)
{
  while (stillReading(input, info.dataOffset + info.dataLength))
  {
    MSPUBBlockInfo subInfo = parseBlock(input, true);
    if (subInfo.id == 0x01)
    {
      m_collector->addPaletteColor(Color(subInfo.data & 0xFF, (subInfo.data >> 8) & 0xFF, (subInfo.data >> 16) & 0xFF));
    }
  }
}

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
