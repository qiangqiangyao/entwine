/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/reader/query.hpp>

#include <iterator>

#include <entwine/reader/cache.hpp>
#include <entwine/reader/chunk-reader.hpp>
#include <entwine/tree/cell.hpp>
#include <entwine/tree/chunk.hpp>
#include <entwine/types/schema.hpp>

namespace entwine
{

namespace
{
    std::size_t fetchesPerIteration(4);
}

BaseQuery::BaseQuery(
        const Reader& reader,
        Cache& cache,
        const BBox& qbox,
        const std::size_t depthBegin,
        const std::size_t depthEnd)
    : m_reader(reader)
    , m_structure(reader.structure())
    , m_cache(cache)
    , m_qbox(qbox)
    , m_depthBegin(depthBegin)
    , m_depthEnd(depthEnd)
    , m_chunks()
    , m_block()
    , m_chunkReaderIt()
    , m_numPoints(0)
    , m_base(true)
    , m_done(false)
{
    if (!m_depthEnd || m_depthEnd > m_structure.coldDepthBegin())
    {
        SplitClimber splitter(
                m_structure,
                m_reader.bbox(),
                m_qbox,
                m_depthBegin,
                m_depthEnd,
                true);

        bool terminate(false);

        do
        {
            terminate = false;
            const Id& chunkId(splitter.index());

            if (reader.exists(chunkId))
            {
                m_chunks.insert(
                        FetchInfo(
                            m_reader,
                            chunkId,
                            m_structure.getInfo(chunkId).chunkPoints(),
                            splitter.depth()));
            }
            else
            {
                terminate = true;
            }
        }
        while (splitter.next(terminate));
    }
}

bool BaseQuery::next()
{
    if (m_done) throw std::runtime_error("Called next after query completed");

    if (m_base)
    {
        m_base = false;

        if (!getBase())
        {
            if (m_chunks.empty()) m_done = true;
            else getChunked();
        }
    }
    else
    {
        getChunked();
    }

    return !m_done;
}

bool BaseQuery::getBase()
{
    bool dataExisted(false);

    if (
            m_reader.base() &&
            m_depthBegin < m_structure.baseDepthEnd() &&
            m_depthEnd   > m_structure.baseDepthBegin())
    {
        const BaseChunk& base(*m_reader.base());
        bool terminate(false);
        SplitClimber splitter(
                m_structure,
                m_reader.bbox(),
                m_qbox,
                m_depthBegin,
                std::min(m_depthEnd, m_structure.baseDepthEnd()));

        if (splitter.index() < m_structure.baseIndexBegin())
        {
            return dataExisted;
        }

        do
        {
            terminate = false;

            const Id& index(splitter.index());
            const Tube& tube(base.getTube(index));

            if (!tube.empty())
            {
                if (processPoint(tube.primaryCell().atom().load()->val()))
                {
                    ++m_numPoints;
                    dataExisted = true;
                }

                for (const auto& c : tube.secondaryCells())
                {
                    if (processPoint(c.second.atom().load()->val()))
                    {
                        ++m_numPoints;
                        dataExisted = true;
                    }
                }
            }
            else
            {
                terminate = true;
            }
        }
        while (splitter.next(terminate));
    }

    return dataExisted;
}

void BaseQuery::getChunked()
{
    if (!m_block)
    {
        if (m_chunks.size())
        {
            const auto begin(m_chunks.begin());
            auto end(m_chunks.begin());
            std::advance(end, std::min(fetchesPerIteration, m_chunks.size()));

            FetchInfoSet subset(begin, end);
            m_block = m_cache.acquire(m_reader.path(), subset);
            m_chunks.erase(begin, end);

            if (m_block) m_chunkReaderIt = m_block->chunkMap().begin();
        }
    }

    if (m_block)
    {
        if (const ChunkReader* cr = m_chunkReaderIt->second)
        {
            ChunkReader::QueryRange range(cr->candidates(m_qbox));
            auto it(range.begin);

            while (it != range.end)
            {
                if (processPoint(it->second)) ++m_numPoints;
                ++it;
            }

            if (++m_chunkReaderIt == m_block->chunkMap().end())
            {
                m_block.reset();
            }
        }
        else
        {
            throw std::runtime_error("Reservation failure");
        }
    }

    m_done = !m_block && m_chunks.empty();
}

Query::Query(
        const Reader& reader,
        const Schema& schema,
        Cache& cache,
        const BBox& qbox,
        const std::size_t depthBegin,
        const std::size_t depthEnd,
        const bool normalize)
    : BaseQuery(reader, cache, qbox, depthBegin, depthEnd)
    , m_buffer(nullptr)
    , m_outSchema(schema)
    , m_normalize(normalize)
    , m_table(reader.schema())
    , m_pointRef(m_table, 0)
{ }

bool Query::next(std::vector<char>& buffer)
{
    if (buffer.size()) throw std::runtime_error("Query buffer not empty");
    m_buffer = &buffer;

    return BaseQuery::next();
}

bool Query::processPoint(const PointInfo& info)
{
    if (!m_buffer) throw std::runtime_error("Query buffer not set");

    if (m_qbox.contains(info.point()))
    {
        m_buffer->resize(m_buffer->size() + m_outSchema.pointSize(), 0);
        char* pos(
                m_buffer->data() + m_buffer->size() - m_outSchema.pointSize());

        m_table.setPoint(info.data());
        bool isX(false), isY(false), isZ(false);

        for (const auto& dim : m_outSchema.dims())
        {
            if (m_normalize)
            {
                isX = dim.id() == pdal::Dimension::Id::X;
                isY = dim.id() == pdal::Dimension::Id::Y;
                isZ = dim.id() == pdal::Dimension::Id::Z;

                if (
                        (isX || isY || isZ) &&
                        pdal::Dimension::size(dim.type()) == 4)
                {
                    double d(m_pointRef.getFieldAs<double>(dim.id()));

                    if (isX)        d -= m_reader.bbox().mid().x;
                    else if (isY)   d -= m_reader.bbox().mid().y;
                    else            d -= m_reader.bbox().mid().z;

                    float f(d);

                    std::memcpy(pos, &f, 4);
                }
                else
                {
                    m_pointRef.getField(pos, dim.id(), dim.type());
                }
            }
            else
            {
                m_pointRef.getField(pos, dim.id(), dim.type());
            }

            pos += dim.size();
        }

        return true;
    }
    else
    {
        return false;
    }
}

bool MetaQuery::processPoint(const PointInfo& info)
{
    const Point& check(info.point());

    m_loBox.set(check - m_radius, check - m_radius, m_is3d);
    m_hiBox.set(check + m_radius, check + m_radius, m_is3d);

    auto it(m_grid.lower_bound(m_loBox));
    auto end(m_grid.upper_bound(m_hiBox));

    while (it != end)
    {
        if (it->first.contains(check))
        {
            ++it->second.numPoints;
            return true;
        }

        ++it;
    }

    return false;
}

} // namespace entwine

