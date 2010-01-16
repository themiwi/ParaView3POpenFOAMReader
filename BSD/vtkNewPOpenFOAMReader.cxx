/*=========================================================================

  Program:   Visualization Toolkit
  Module:    $RCSfile: vtkNewPOpenFOAMReader.cxx,v $

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See License_v1.2.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
// This class was developed by Takuya Oshima at Niigata University,
// Japan (oshima@eng.niigata-u.ac.jp).

#include "vtkNewPOpenFOAMReader.h"

#include "vtkAppendCompositeDataLeaves.h"
#include "vtkCharArray.h"
#include "vtkCollection.h"
#include "vtkCompositeDataIterator.h"
#include "vtkDataArraySelection.h"
#include "vtkDirectory.h"
#include "vtkDoubleArray.h"
#include "vtkFieldData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkIntArray.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkMultiProcessController.h"
#include "vtkObjectFactory.h"
#include "vtkSortDataArray.h"
#include "vtkStdString.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStringArray.h"

vtkCxxRevisionMacro(vtkNewPOpenFOAMReader, "$Revision: 1.1 $");
vtkStandardNewMacro(vtkNewPOpenFOAMReader);
vtkCxxSetObjectMacro(vtkNewPOpenFOAMReader, Controller, vtkMultiProcessController);

//-----------------------------------------------------------------------------
vtkNewPOpenFOAMReader::vtkNewPOpenFOAMReader()
{
  this->Controller = NULL;
  this->SetController(vtkMultiProcessController::GetGlobalController());
  if (this->Controller == NULL)
    {
    this->NumProcesses = 1;
    this->ProcessId = 0;
    }
  else
    {
    this->NumProcesses = this->Controller->GetNumberOfProcesses();
    this->ProcessId = this->Controller->GetLocalProcessId();
    }
  this->CaseType = RECONSTRUCTED_CASE;
  this->MTimeOld = 0;
  this->MaximumNumberOfPieces = 1;

  this->UiInterval = 60;
  this->UiRescale = 1;
  this->UiWatch = 0;
}

//-----------------------------------------------------------------------------
vtkNewPOpenFOAMReader::~vtkNewPOpenFOAMReader()
{
  this->SetController(NULL);
}

//-----------------------------------------------------------------------------
void vtkNewPOpenFOAMReader::PrintSelf(ostream &os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "Controller: " << this->Controller << endl;
  os << indent << "Case Type: " << this->CaseType << endl;
  os << indent << "MTimeOld: " << this->MTimeOld << endl;
  os << indent << "Maximum Number of Pieces: " << this->MaximumNumberOfPieces
      << endl;
  os << indent << "Number of Processes: " << this->NumProcesses << endl;
  os << indent << "Process Id: " << this->ProcessId << endl;
  os << indent << "Ui Interval: " << this->UiInterval;
  os << indent << "Ui Rescale: " << this->UiRescale;
  os << indent << "Ui Watch: " << this->UiWatch;
}

//-----------------------------------------------------------------------------
void vtkNewPOpenFOAMReader::SetCaseType(const int t)
{
  if (this->CaseType != t)
    {
    this->CaseType = static_cast<caseType>(t);
    this->Refresh = true;
    this->Modified();
    }
}

//-----------------------------------------------------------------------------
int vtkNewPOpenFOAMReader::RequestInformation(vtkInformation *request,
    vtkInformationVector **inputVector, vtkInformationVector *outputVector)
{
  if (this->CaseType == RECONSTRUCTED_CASE)
    {
    int ret = 1;
    if (this->ProcessId == 0)
      {
      ret = this->Superclass::RequestInformation(request, inputVector,
          outputVector);
      }
    if (this->NumProcesses > 1)
      {
      // if there was an error in process 0 abort all processes
      this->BroadcastStatus(ret);
      if (ret == 0)
        {
        vtkErrorMacro(<< "The master process returned an error.");
        return 0;
        }

      vtkDoubleArray *timeValues;
      if (this->ProcessId == 0)
        {
        timeValues = this->Superclass::GetTimeValues();
        }
      else
        {
        timeValues = vtkDoubleArray::New();
        }
      this->Controller->Broadcast(timeValues, 0);
      if (this->ProcessId != 0)
        {
        this->Superclass::SetTimeInformation(outputVector, timeValues);
        timeValues->Delete();
        this->Superclass::Refresh = false;
        }
      this->Controller->Broadcast(this->CasePath, 0);
      this->GatherMetaData(); // pvserver deadlocks without this
      }

    return ret;
    }

  if (!this->Superclass::FileName || strlen(this->Superclass::FileName) == 0)
    {
    vtkErrorMacro("FileName has to be specified!");
    return 0;
    }

  if (*this->Superclass::FileNameOld != this->Superclass::FileName
      || this->Superclass::ListTimeStepsByControlDict
          != this->Superclass::ListTimeStepsByControlDictOld
      || this->Superclass::Refresh)
    {
    // retain selection statuses when just refreshing a case
    if (*this->Superclass::FileNameOld != "" && *this->Superclass::FileNameOld != this->Superclass::FileName)
      {
      // clear selections
      this->Superclass::CellDataArraySelection->RemoveAllArrays();
      this->Superclass::PointDataArraySelection->RemoveAllArrays();
      this->Superclass::LagrangianDataArraySelection->RemoveAllArrays();
      this->Superclass::PatchDataArraySelection->RemoveAllArrays();
      }

    *this->Superclass::FileNameOld = vtkStdString(this->FileName);
    this->Superclass::Readers->RemoveAllItems();
    this->Superclass::NumberOfReaders = 0;

    vtkStringArray *procNames = vtkStringArray::New();
    vtkDoubleArray *timeValues;

    // recreate case information
    vtkStdString masterCasePath, controlDictPath;
    this->Superclass::CreateCasePath(masterCasePath, controlDictPath);

    this->Superclass::CreateCharArrayFromString(this->Superclass::CasePath,
        "CasePath", masterCasePath);

    int ret = 1;
    if (this->ProcessId == 0)
      {
      // search and list processor subdirectories
      vtkDirectory *dir = vtkDirectory::New();
      if (!dir->Open(masterCasePath.c_str()))
        {
        vtkErrorMacro(<< "Can't open " << masterCasePath.c_str());
        dir->Delete();
        this->BroadcastStatus(ret = 0);
        return 0;
        }
      vtkIntArray *procNos = vtkIntArray::New();
      for (int fileI = 0; fileI < dir->GetNumberOfFiles(); fileI++)
        {
        const vtkStdString subDir(dir->GetFile(fileI));
        if (subDir.substr(0, 9) == "processor")
          {
          const vtkStdString procNoStr(subDir.substr(9, vtkStdString::npos));
          char *conversionEnd;
          const int procNo = strtol(procNoStr.c_str(), &conversionEnd, 10);
          if (procNoStr.c_str() + procNoStr.length() == conversionEnd && procNo
              >= 0)
            {
            procNos->InsertNextValue(procNo);
            procNames->InsertNextValue(subDir);
            }
          }
        }
      procNos->Squeeze();
      procNames->Squeeze();
      dir->Delete();

      // sort processor subdirectories by processor numbers
      vtkSortDataArray::Sort(procNos, procNames);
      procNos->Delete();

      // get time directories from the first processor subdirectory
      if (procNames->GetNumberOfTuples() > 0)
        {
        vtkNewOpenFOAMReader *masterReader = vtkNewOpenFOAMReader::New();
        masterReader->SetFileName(this->FileName);
        masterReader->SetParent(this);
        if (!masterReader->MakeInformationVector(outputVector, procNames
        ->GetValue(0)) || !masterReader->MakeMetaDataAtTimeStep(true))
          {
          procNames->Delete();
          masterReader->Delete();
          this->BroadcastStatus(ret = 0);
          return 0;
          }
        this->Superclass::Readers->AddItem(masterReader);
        timeValues = masterReader->GetTimeValues();
        masterReader->Delete();
        }
      else
        {
        timeValues = vtkDoubleArray::New();
        this->Superclass::SetTimeInformation(outputVector, timeValues);
        }
      }
    else
      {
      timeValues = vtkDoubleArray::New();
      }

    if (this->NumProcesses > 1)
      {
      // if there was an error in process 0 abort all processes
      this->BroadcastStatus(ret);
      if (ret == 0)
        {
        vtkErrorMacro(<< "The master process returned an error.");
        timeValues->Delete(); // don't have to care about process 0
        return 0;
        }

      this->Broadcast(procNames);
      this->Controller->Broadcast(timeValues, 0);
      if (this->ProcessId != 0)
        {
        this->Superclass::SetTimeInformation(outputVector, timeValues);
        timeValues->Delete();
        }
      }

    if (this->ProcessId == 0 && procNames->GetNumberOfTuples() == 0)
      {
      vtkWarningMacro(<< " Case " << this->CasePath->GetPointer(0) << " contains no processor subdirectory.");
      timeValues->Delete();
      }

    this->MaximumNumberOfPieces = procNames->GetNumberOfTuples();

    // create reader instances for other processor subdirectories
    // skip processor0 since it's already been created
    for (int procI = (this->ProcessId ? this->ProcessId : this->NumProcesses); procI
        < procNames->GetNumberOfTuples(); procI += this->NumProcesses)
      {
      vtkNewOpenFOAMReader *subReader = vtkNewOpenFOAMReader::New();
      subReader->SetFileName(this->FileName);
      subReader->SetParent(this);
      // if getting metadata failed simply delete the reader instance
      if (subReader->MakeInformationVector(NULL, procNames->GetValue(procI))
          && subReader->MakeMetaDataAtTimeStep(true))
        {
        this->Superclass::Readers->AddItem(subReader);
        }
      else
        {
        vtkWarningMacro(<<"Removing reader for processor subdirectory "
            << procNames->GetValue(procI).c_str());
        }
      subReader->Delete();
      }

    procNames->Delete();

    this->GatherMetaData();
    this->Superclass::Refresh = false;
    }

  // it seems MAXIMUM_NUMBER_OF_PIECES must be returned every time
  // RequestInformation() is called
  outputVector->GetInformationObject(0)->Set(vtkStreamingDemandDrivenPipeline::MAXIMUM_NUMBER_OF_PIECES(),
      this->MaximumNumberOfPieces);

  return 1;
}

//-----------------------------------------------------------------------------
int vtkNewPOpenFOAMReader::RequestData(vtkInformation *request,
    vtkInformationVector **inputVector, vtkInformationVector *outputVector)
{
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  vtkMultiBlockDataSet
      *output =
          vtkMultiBlockDataSet::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  int ret = 1;

  if (this->CaseType == RECONSTRUCTED_CASE)
    {
    if (this->ProcessId == 0)
      {
      ret = this->Superclass::RequestData(request, inputVector, outputVector);
      }

    this->BroadcastStructure(output, 1);
    if (this->ProcessId > 0)
      {
      output->GetFieldData()->AddArray(this->Superclass::CasePath);
      }

    this->BroadcastStatus(ret);
    this->GatherMetaData();

    return ret;
    }

  int nSteps = 0;
  double *requestedTimeValues = NULL;
  if (outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEPS()))
    {
    requestedTimeValues
      = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEPS());
    nSteps = outInfo->Length(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
    if (nSteps > 0)
      {
      outInfo->Set(vtkDataObject::DATA_TIME_STEPS(), requestedTimeValues, 1);
      }
    }

  const int nReaders = this->Superclass::Readers->GetNumberOfItems();
  if (nReaders > 0)
    {
    vtkAppendCompositeDataLeaves *append = (nReaders > 1
        ? vtkAppendCompositeDataLeaves::New() : NULL);
    // append->AppendFieldDataOn();

    vtkNewOpenFOAMReader *reader;
    this->Superclass::CurrentReaderIndex = 0;
    this->Superclass::Readers->InitTraversal();
    while ((reader
        = vtkNewOpenFOAMReader::SafeDownCast(this->Superclass::Readers->GetNextItemAsObject()))
        != NULL)
      {
      // even if the child readers themselves are not modified, mark
      // them as modified if "this" has been modified, since they
      // refer to the property of "this"
      if ((nSteps > 0 && reader->SetTimeValue(requestedTimeValues[0]))
          || this->MTimeOld != this->GetMTime())
        {
        reader->Modified();
        }
      if ((ret = reader->MakeMetaDataAtTimeStep(false)) && nReaders > 1)
        {
        append->AddInputConnection(reader->GetOutputPort());
        }
      }

    this->GatherMetaData();

    if (nReaders == 1)
      {
      if(ret)
        {
        reader = vtkNewOpenFOAMReader::SafeDownCast(
            this->Superclass::Readers->GetItemAsObject(0));
        // reader->RequestInformation() and RequestData() are called
        // for all reader instances without setting UPDATE_TIME_STEPS.
        // reader->GetExecutive()->Update() is used because
        // reader->Update() doesn't return a value.
        ret = reader->GetExecutive()->Update();
        output->ShallowCopy(reader->GetOutput());
        }
      }
    else
      {
      if (append->GetInput() != NULL)
        {
        // reader->RequestInformation() and RequestData() are called
        // for all reader instances without setting UPDATE_TIME_STEPS.
        // reader->GetExecutive()->Update() is used because
        // reader->Update() doesn't return a value.
        ret = append->GetExecutive()->Update();
        output->ShallowCopy(append->GetOutput());
        }
      append->Delete();
      }

    if(!ret)
      {
      output->Initialize();
      }
    }
  else
    {
    this->GatherMetaData();
    // page 322 of The ParaView Guide says the output must be initialized
    output->Initialize();
    }

  output->GetFieldData()->AddArray(this->Superclass::CasePath);

  if (this->MaximumNumberOfPieces < this->NumProcesses)
    {
    this->BroadcastStructure(output, this->MaximumNumberOfPieces);
    }

  this->Superclass::UpdateStatus();
  this->MTimeOld = this->GetMTime();

  return ret;
}

//-----------------------------------------------------------------------------
void vtkNewPOpenFOAMReader::BroadcastStatus(int &status)
{
  if (this->NumProcesses > 1)
    {
    this->Controller->Broadcast(&status, 1, 0);
    }
}

//-----------------------------------------------------------------------------
void vtkNewPOpenFOAMReader::GatherMetaData()
{
  if (this->NumProcesses > 1)
    {
    this->AllGather(this->Superclass::PatchDataArraySelection);
    this->AllGather(this->Superclass::CellDataArraySelection);
    this->AllGather(this->Superclass::PointDataArraySelection);
    this->AllGather(this->Superclass::LagrangianDataArraySelection);
    // omit removing duplicated entries of LagrangianPaths as well
    // when the number of processes is 1 assuming there's no duplicate
    // entry within a process
    this->AllGather(this->Superclass::LagrangianPaths);
    }
}

//-----------------------------------------------------------------------------
// Decode and reconstruct the encoded multiblock structure. Helper
// function for BroadcastStructure().
int vtkNewPOpenFOAMReader::ConstructBlocks(vtkMultiBlockDataSet *ds, int *dataTypes,
    int leafI)
{
  ds->SetNumberOfBlocks(dataTypes[leafI]);
  for (unsigned int blockI = 0; blockI < ds->GetNumberOfBlocks(); blockI++)
    {
    if (dataTypes[++leafI] >= 0)
      {
      vtkMultiBlockDataSet *mbds = ds->NewInstance();
      leafI = this->ConstructBlocks(mbds, dataTypes, leafI);
      ds->SetBlock(blockI, mbds);
      mbds->Delete();
      }
    }
  return leafI;
}

//-----------------------------------------------------------------------------
// Broadcast the structure of a multiblock dataset. Everything has to
// be self-coded because
// vtkCommunicator::{Send,Receive}MultiBlockDataSet()s in ParaView
// 3.6.1 are broken (whereas those in 3.7-cvs have been fixed).
void vtkNewPOpenFOAMReader::BroadcastStructure(vtkMultiBlockDataSet *ds, const int startProc)
{
  if (this->NumProcesses <= 1)
    {
    return;
    }

  if (this->ProcessId == 0)
    {
    vtkCompositeDataIterator *iter = ds->NewIterator();
    iter->SkipEmptyNodesOff();
    iter->VisitOnlyLeavesOff();

    // count the number of leaves and allocate buffer
    int nLeaves = 1; // in order to accommodate the number of blocks of the top
    for (iter->InitTraversal(); !iter->IsDoneWithTraversal();
        iter->GoToNextItem())
      {
      nLeaves++;
      }
    int *dataTypes = new int [nLeaves];

    // encode the multiblock structure
    dataTypes[0] = ds->GetNumberOfBlocks();
    int leafI = 1;
    for (iter->InitTraversal(); !iter->IsDoneWithTraversal();
        iter->GoToNextItem(), leafI++)
      {
      vtkDataObject *leaf = iter->GetCurrentDataObject();
      vtkMultiBlockDataSet *mbds;
      if (leaf != NULL
          && (mbds = vtkMultiBlockDataSet::SafeDownCast(leaf)) != NULL)
        {
        dataTypes[leafI] = static_cast<int>(mbds->GetNumberOfBlocks());
        }
      else
        {
        dataTypes[leafI] = -1;
        }
      }
    iter->Delete();

    // broadcast
    for (int procI = startProc; procI < this->NumProcesses; procI++)
      {
      this->Controller->Send(&nLeaves, 1, procI, 101);
      this->Controller->Send(dataTypes, nLeaves, procI, 102);
      }
    delete [] dataTypes;
    }
  else if (this->ProcessId >= startProc)
    {
    int nLeaves;
    this->Controller->Receive(&nLeaves, 1, 0, 101);
    int *dataTypes = new int [nLeaves];
    this->Controller->Receive(dataTypes, nLeaves, 0, 102);
    ds->Initialize();
    this->ConstructBlocks(ds, dataTypes, 0);
    delete [] dataTypes;
    }
}

//-----------------------------------------------------------------------------
// Broadcast a vtkStringArray in process 0 to all processes
void vtkNewPOpenFOAMReader::Broadcast(vtkStringArray *sa)
{
  vtkIdType lengths[2];
  if (this->ProcessId == 0)
    {
    lengths[0] = sa->GetNumberOfTuples();
    lengths[1] = 0;
    for (int strI = 0; strI < sa->GetNumberOfTuples(); strI++)
      {
      lengths[1] += static_cast<vtkIdType>(sa->GetValue(strI).length()) + 1;
      }
    }
  this->Controller->Broadcast(lengths, 2, 0);
  char *contents = new char [lengths[1]];
  if (this->ProcessId == 0)
    {
    for (int strI = 0, idx = 0; strI < sa->GetNumberOfTuples(); strI++)
      {
      const int len = static_cast<int>(sa->GetValue(strI).length()) + 1;
      memmove(contents + idx, sa->GetValue(strI).c_str(), len);
      idx += len;
      }
    }
  this->Controller->Broadcast(contents, lengths[1], 0);
  if (this->ProcessId != 0)
    {
    sa->Initialize();
    sa->SetNumberOfTuples(lengths[0]);
    for (int strI = 0, idx = 0; strI < lengths[0]; strI++)
      {
      sa->SetValue(strI, contents + idx);
      idx += static_cast<int>(sa->GetValue(strI).length()) + 1;
      }
    }
  delete [] contents;
}

//-----------------------------------------------------------------------------
// AllGather vtkStringArrays from and to all processes
void vtkNewPOpenFOAMReader::AllGather(vtkStringArray *s)
{
  vtkIdType length = 0;
  for (int strI = 0; strI < s->GetNumberOfTuples(); strI++)
    {
    length += static_cast<vtkIdType>(s->GetValue(strI).length()) + 1;
    }
  vtkIdType *lengths = new vtkIdType [this->NumProcesses];
  this->Controller->AllGather(&length, lengths, 1);
  vtkIdType totalLength = 0;
  vtkIdType *offsets = new vtkIdType [this->NumProcesses];
  for (int procI = 0; procI < this->NumProcesses; procI++)
    {
    offsets[procI] = totalLength;
    totalLength += lengths[procI];
    }
  char *allContents = new char [totalLength], *contents = new char [length];
  for (int strI = 0, idx = 0; strI < s->GetNumberOfTuples(); strI++)
    {
    const int len = static_cast<int>(s->GetValue(strI).length()) + 1;
    memmove(contents + idx, s->GetValue(strI).c_str(), len);
    idx += len;
    }
  this->Controller->AllGatherV(contents, allContents, length, lengths, offsets);
  delete [] contents;
  delete [] lengths;
  delete [] offsets;
  s->Initialize();
  for (int idx = 0; idx < totalLength; idx += static_cast<int>(strlen(allContents + idx)) + 1)
    {
    const char *str = allContents + idx;
    // insert only when the same string is not found
    if (s->LookupValue(str) == -1)
      {
      s->InsertNextValue(str);
      }
    }
  s->Squeeze();
  delete [] allContents;
}

//-----------------------------------------------------------------------------
// AllGather vtkDataArraySelections from and to all processes
void vtkNewPOpenFOAMReader::AllGather(vtkDataArraySelection *s)
{
  vtkIdType length = 0;
  for (int strI = 0; strI < s->GetNumberOfArrays(); strI++)
    {
    length += static_cast<vtkIdType>(strlen(s->GetArrayName(strI))) + 2;
    }
  vtkIdType *lengths = new vtkIdType [this->NumProcesses];
  this->Controller->AllGather(&length, lengths, 1);
  vtkIdType totalLength = 0;
  vtkIdType *offsets = new vtkIdType [this->NumProcesses];
  for (int procI = 0; procI < this->NumProcesses; procI++)
    {
    offsets[procI] = totalLength;
    totalLength += lengths[procI];
    }
  char *allContents = new char [totalLength], *contents = new char [length];
  for (int strI = 0, idx = 0; strI < s->GetNumberOfArrays(); strI++)
    {
    const char *arrayName = s->GetArrayName(strI);
    contents[idx] = s->ArrayIsEnabled(arrayName);
    const int len = static_cast<int>(strlen(arrayName)) + 1;
    memmove(contents + idx + 1, arrayName, len);
    idx += len + 1;
    }
  this->Controller->AllGatherV(contents, allContents, length, lengths, offsets);
  delete [] contents;
  delete [] lengths;
  delete [] offsets;
  // do not RemoveAllArray so that the previous arrays are preserved
  // s->RemoveAllArrays();
  for (int idx = 0; idx < totalLength; idx += static_cast<int>(strlen(allContents + idx + 1)) + 2)
    {
    const char *arrayName = allContents + idx + 1;
    s->AddArray(arrayName);
    if (allContents[idx] == 0)
      {
      s->DisableArray(arrayName);
      }
    else
      {
      s->EnableArray(arrayName);
      }
    }
  delete [] allContents;
}
