import { Component, ViewChild, AfterViewInit } from '@angular/core';
import { FormGroup } from '@angular/forms';
import { Observable, asyncScheduler } from 'rxjs';
import { observeOn } from 'rxjs/operators';
import { EditComponent } from '../edit/edit.component';

@Component({
  selector: 'app-settings',
  templateUrl: './settings.component.html',
})
export class SettingsComponent implements AfterViewInit {
  form$!: Observable<FormGroup | null>;

  @ViewChild(EditComponent) editComponent!: EditComponent;

  constructor() {}

  ngAfterViewInit() {
    // observeOn(asyncScheduler): form$ is only available after view init, so
    // emissions are deferred one tick to avoid NG0100 (value changing within
    // the same change-detection cycle). Display timing only.
    this.form$ = this.editComponent.form$.pipe(observeOn(asyncScheduler));
  }
}
