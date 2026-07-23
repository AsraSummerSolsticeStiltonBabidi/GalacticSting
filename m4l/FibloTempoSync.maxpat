{
	"patcher" : 	{
		"fileversion" : 1,
		"appversion" : 		{
			"major" : 8,
			"minor" : 6,
			"revision" : 0,
			"architecture" : "x64",
			"modernui" : 1
		}
,
		"classnamespace" : "box",
		"rect" : [ 34.0, 79.0, 640.0, 480.0 ],
		"bglocked" : 0,
		"openinpresentation" : 1,
		"default_fontsize" : 12.0,
		"default_fontface" : 0,
		"default_fontname" : "Arial",
		"gridonopen" : 1,
		"gridsize" : [ 8.0, 8.0 ],
		"gridsnaponopen" : 1,
		"objectsnaponopen" : 1,
		"statusbarvisible" : 2,
		"toolbarvisible" : 1,
		"lefttoolbarpinned" : 0,
		"toptoolbarpinned" : 0,
		"righttoolbarpinned" : 0,
		"bottomtoolbarpinned" : 0,
		"toolbars_unpinned_last_save" : 0,
		"tallnewobj" : 0,
		"boxanimatetime" : 500,
		"enablehscroll" : 1,
		"enablevscroll" : 1,
		"devicewidth" : 0.0,
		"description" : "",
		"digest" : "",
		"tags" : "",
		"style" : "",
		"subpatcher_template" : "",
		"title" : "Max MIDI Effect",
		"boxes" : [ 			{
				"box" : 				{
					"id" : "obj-title",
					"maxclass" : "comment",
					"fontname" : "Arial Bold",
					"fontsize" : 13.0,
					"numinlets" : 1,
					"numoutlets" : 0,
					"patching_rect" : [ 24.0, 16.0, 300.0, 21.0 ],
					"presentation" : 1,
					"presentation_rect" : [ 8.0, 6.0, 240.0, 21.0 ],
					"text" : "Fibló → DAW Tempo Sync"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-sub",
					"maxclass" : "comment",
					"fontname" : "Arial",
					"fontsize" : 10.0,
					"numinlets" : 1,
					"numoutlets" : 0,
					"patching_rect" : [ 24.0, 36.0, 360.0, 18.0 ],
					"presentation" : 1,
					"presentation_rect" : [ 8.0, 28.0, 250.0, 32.0 ],
					"linecount" : 2,
					"text" : "Sets Live's tempo from the Fibló's CC20 (BPM = value + 20). No mapping needed."
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-midiin",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 1,
					"numoutlets" : 1,
					"outlettype" : [ "int" ],
					"patching_rect" : [ 40.0, 80.0, 48.0, 22.0 ],
					"text" : "midiin"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-midiout",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 1,
					"numoutlets" : 0,
					"patching_rect" : [ 200.0, 120.0, 54.0, 22.0 ],
					"text" : "midiout"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-parse",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 1,
					"numoutlets" : 8,
					"outlettype" : [ "", "", "", "", "", "", "", "" ],
					"patching_rect" : [ 40.0, 120.0, 140.0, 22.0 ],
					"text" : "midiparse"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-route",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 1,
					"numoutlets" : 2,
					"outlettype" : [ "", "" ],
					"patching_rect" : [ 40.0, 160.0, 64.0, 22.0 ],
					"text" : "route 20"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-add",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 2,
					"numoutlets" : 1,
					"outlettype" : [ "int" ],
					"patching_rect" : [ 40.0, 200.0, 44.0, 22.0 ],
					"text" : "+ 20"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-prepend",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 1,
					"numoutlets" : 1,
					"outlettype" : [ "" ],
					"patching_rect" : [ 40.0, 240.0, 116.0, 22.0 ],
					"text" : "prepend set tempo"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-liveobject",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 2,
					"numoutlets" : 1,
					"outlettype" : [ "" ],
					"patching_rect" : [ 40.0, 320.0, 84.0, 22.0 ],
					"text" : "live.object",
					"saved_object_attributes" : 					{
						"_persistence" : 0
					}

				}

			}
, 			{
				"box" : 				{
					"id" : "obj-livepath",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 1,
					"numoutlets" : 3,
					"outlettype" : [ "", "", "" ],
					"patching_rect" : [ 300.0, 240.0, 72.0, 22.0 ],
					"text" : "live.path"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-msg",
					"maxclass" : "message",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 2,
					"numoutlets" : 1,
					"outlettype" : [ "" ],
					"patching_rect" : [ 300.0, 200.0, 104.0, 22.0 ],
					"text" : "path live_set"
				}

			}
, 			{
				"box" : 				{
					"id" : "obj-thisdevice",
					"maxclass" : "newobj",
					"fontname" : "Arial",
					"fontsize" : 12.0,
					"numinlets" : 1,
					"numoutlets" : 3,
					"outlettype" : [ "", "", "" ],
					"patching_rect" : [ 300.0, 160.0, 108.0, 22.0 ],
					"text" : "live.thisdevice"
				}

			}
 ],
		"lines" : [ 			{
				"patchline" : 				{
					"source" : [ "obj-midiin", 0 ],
					"destination" : [ "obj-parse", 0 ]
				}

			}
, 			{
				"patchline" : 				{
					"source" : [ "obj-midiin", 0 ],
					"destination" : [ "obj-midiout", 0 ]
				}

			}
, 			{
				"patchline" : 				{
					"source" : [ "obj-parse", 2 ],
					"destination" : [ "obj-route", 0 ]
				}

			}
, 			{
				"patchline" : 				{
					"source" : [ "obj-route", 0 ],
					"destination" : [ "obj-add", 0 ]
				}

			}
, 			{
				"patchline" : 				{
					"source" : [ "obj-add", 0 ],
					"destination" : [ "obj-prepend", 0 ]
				}

			}
, 			{
				"patchline" : 				{
					"source" : [ "obj-prepend", 0 ],
					"destination" : [ "obj-liveobject", 0 ]
				}

			}
, 			{
				"patchline" : 				{
					"source" : [ "obj-thisdevice", 0 ],
					"destination" : [ "obj-msg", 0 ]
				}

			}
, 			{
				"patchline" : 				{
					"source" : [ "obj-msg", 0 ],
					"destination" : [ "obj-livepath", 0 ]
				}

			}
, 			{
				"patchline" : 				{
					"source" : [ "obj-livepath", 0 ],
					"destination" : [ "obj-liveobject", 1 ]
				}

			}
 ],
		"dependency_cache" : [  ],
		"latency" : 0,
		"project" : 		{
			"version" : 1,
			"creationdate" : 3605254137,
			"modificationdate" : 3605254137,
			"viewrect" : [ 0.0, 0.0, 300.0, 500.0 ],
			"autoorganize" : 1,
			"hideprojectwindow" : 1,
			"showdependencies" : 1,
			"autolocalize" : 0,
			"contents" : 			{
				"patchers" : 				{

				}

			}
,
			"layout" : 			{

			}
,
			"searchpath" : 			{

			}
,
			"detailsvisible" : 0,
			"amxdtype" : 1836411501,
			"readonly" : 0,
			"devpathtype" : 0,
			"devpath" : ".",
			"sortmode" : 0
		}
,
		"autosave" : 0
	}

}
